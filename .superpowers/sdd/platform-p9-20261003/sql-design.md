# SQL workstream design (P9): JSON artifacts to SQLite, Python orchestration toward engine C++

Status: design for the PM, revision 2. Rulings SQL-1..SQL-3 and **SQL-4..SQL-9** (`progress.md`) bind it; revision 2
applies SQL-4..SQL-9 and supersedes revision 1 where they differ (notably: the table generator is compile-time C++,
SQL-5). Read-only inventory of `C:/atx-wt/pool-2` (v8 head `5292b46e`) plus the wave-1 lane heads that the post-wave-1
head will contain (`@e1` = `feat/p9-e1-20261003` `fdd2bda3`, `@a2` = `2b6f8e6f`, `@b1` = `01f20405`, `@s1` =
`34ef92dd`). No build, no run, no statistic read: from `build-equity/` only file names, counts, sizes, JSON key
structure and line-end bytes were read.

Basis tags on every claim: **[c file:line]** read in code; **[m]** measured on disk (names / counts / sizes only);
**[w url]** sqlite.org documentation; **[j]** design judgment; **[r SQL-n]** a PM ruling.

Companion files: `sql-plan-amendment.md` (exact plan text), `briefs/brief-SQL1.md` .. `brief-SQL4.md`.

---

## 0. Findings that change the framing

1. **SQLite is already in the tree, vendored, and wrapped.** `atx-core/third-party/sqlite` holds the 3.53.2
   amalgamation, compiled as static target `atx_sqlite3` and linked PRIVATE into `atx-core` with hardening defines
   (`SQLITE_DQS=0`, `DEFAULT_FOREIGN_KEYS=1`, `THREADSAFE=2`, FTS5) [c atx-core/CMakeLists.txt:1-27, 69-78, 116];
   provenance and SHA-256 pinned [c atx-core/third-party/sqlite/PROVENANCE.md]. An RAII / `Result` wrapper exists:
   `atx::core::db::{Database, Statement, Transaction, BlobStream}` with `begin_immediate`, online backup and a
   statement cache [c atx-core/include/atx/core/db/sqlite.hpp:100-260; atx-core/src/db/sqlite.cpp], tested by 23
   gtests [c atx-core/tests/db_sqlite_test.cpp:60-432]. Ruled: no new SQLite dependency; lanes link `atx_sqlite3` and
   extend `atx/core/db`; a second copy in one process is a corruption cause [r SQL-4]
   [w https://sqlite.org/howtocorrupt.html §2.3].
2. A second SQLite user exists: the header-only alpha-lifecycle store `atx/engine/store/` (WAL + `synchronous=NORMAL`,
   `CREATE TABLE IF NOT EXISTS` DDL, a `schema_meta` row re-stamped to v2 on every open) [c atx-engine/include/atx/
   engine/store/db.hpp:40-47; schema.hpp:14-177]. Different domain, and its versioning cannot evolve a column
   (re-stamp, no migration) [c schema.hpp:150-175]. The research store is a separate module and does not edit it [j].
3. Python's stdlib `sqlite3` on the host is SQLite **3.43.1** (Python 3.12.2) [m]; the vendored C library is 3.53.2.
   Only C++ creates or migrates a store; Python opens stores C++ made (test fixtures built by the test-only
   `make_store.py` aside). Both read STRICT tables (3.37+)
   [w https://sqlite.org/stricttables.html] and JSON1; the DDL the templates emit must use nothing newer than 3.43.1
   (no `jsonb`), checked by a pytest that applies the printed DDL on Python's SQLite [j].
4. The writers' bytes depend on Windows text mode: bounded-run `start.json` / `receipt.json` and `cycle_binding.json`
   are written without `newline="\n"` and carry CRLF; stage receipts, verdicts, wave results and the trial ledger are
   LF [c rbr@e1:434, 497-499; cycle_resume@e1:189; stage_chain@e1:273-274; cycle_verdict@e1:146-147;
   wave_context@e1:72-78] [m: CR count = LF count in a receipt (138/138), a binding (7/7); CR 0 in `05-run.json`, a
   verdict, `wave-result.json`, `trials.jsonl`]. The byte-identity checker models the line end per file (§3.9).
5. The IC exe and the marginal verb write their cache state into output bytes: the u / w summary carries a
   `candidate_cache` block (layout, hits, legacy hits, misses, IC-result subdirectory and hits) [c
   strategy_ic_runner.cpp:583-596] and `marginal_ic.json` a `pair_cache` block (directory, key, shards read, hits,
   computed) [c strategy_marginal_ic.cpp@s1:1001-1014, 1048]. The fitter and the report card write no cache state
   [c fit_composition_weights.py:1293-1325; alpha_report_card.py:743-755]. This decides presence vs flag per cache
   under SQL-6 (§3.10).
6. Nothing in P9 may move a pinned byte [r SQL-2]. Every class that a pin, receipt chain or the ledger chain covers
   keeps JSON authority through the freeze; inside P9 only **unpinned caches** reach "JSON retired" (stage 3) (§4).

---

## 1. (a) Artifact inventory

Kinds: **IN** = input authored by a human or by a tool on a human's command, git-tracked, reviewed in diffs;
**OUT** = output record of a run; **CACHE** = content-keyed, safe to delete, never a research value; **PAY** = numerical
payload file. Counts/sizes [m] are for `build-equity/` of pool-2 excluding the CMake build dirs (~15,000 files; 988
`*.f64` = 62.0 GB; 7,665 `*.json` = 334 MB; 2,320 `*.csv` = 1.83 GB; 119 `*.jsonl` = 20 MB).

### 1.1 Inputs (git-tracked)

| # | class (schema) | where, count, size [m] | writer | readers | covered by |
|---|---|---|---|---|---|
| I1 | cycle spec `atx.research-cycle-spec/v1` (+ `atx.mine-campaign-spec/v1`, `atx.library-spec/v1`) | `scripts/specs/**` 25 + 1 mine spec (86 JSON, 397 KB in all of `scripts/specs/`); `atx-impl/strategies/specs` 1 | human; `lock --write` rewrites pins [c research_cycle@e1:2268]; cell specs by the wave [c wave_stage_util@e1:86-97]; `research_add_alpha.py:374,390`; `research_mine.py:751-753` | `research_cycle` load / resolve, `wave_context.load_spec` [c wave_context@e1:101-108] | git commit; `spec_digest` recorded in verdict + binding [c research_spec.py:35-39]; pre-registration commits |
| I2 | cycle template `atx.research-cycle-template/v1` | 40 files (`v8` 32, `v8-rerun` 8), ~1-6 KB each | human / `research_add_alpha.py` | `research_spec.resolve` | git; chain digest over every parent [c research_spec.py:35-39]; `locked` pins |
| I3 | wave manifest `atx.research-wave/v1` | `scripts/specs/v8/waves` 5 (y-s 24 KB) | human; `wave_queue emit` [c wave_queue@e1:344] | `wave_manifest.load` [c wave_context@e1:42] | git; manifest SHA in `wave-result.manifest` and stage receipts |
| I4 | candidate registration `atx.wave-candidate/v1` | `scripts/specs/v8/candidates` 15, ~2 KB | `wave_queue new/pin` and the record stage [c wave_queue@e1:141, 195] | `wave_queue validate/list/emit` | git; DSL SHA-256 inside; status history |
| I5 | IC library `atx.dsl-ic-library/v1`, recipe `atx.dsl-ic-experiment/v1|v2`, `libraries/*.json` | `atx-impl/strategies` 77 JSON (58 MB dir incl. Python) | `generate_library.py`, `generate_from_spec.py`, the wave library stage [c wave_stage_library@e1:34] | IC exe, fitter, specs | pinned in specs `inputs.library/recipe.sha256` [m: key structure of `lib-v8ysb-gm.json`] |
| I6 | alpha registry `atx.alpha-registry/v1` | `atx-impl/strategies/alphas/registry.json` 187.5 KB | human | `wave_queue` [c wave_queue@e1:45], D1 theme table | git |
| I7 | research window `atx.research-window/v2` | 1 | human (owner ruling) | `research_window.hpp`, Python seal code | SHA in ledger protocol lines [c research_ledger.py:25-26] |
| I8 | field registry `atx.field-registry/v1` (K-P9-1) | `atx-engine/tools/field_registry.json` (A1, 3,348 lines) | A1 `field_registry.py` generator | A2 C++ reader, A1 builder entry | pinned in the A2 manifest `registry {path, sha256}` [c manifest.hpp@a2:15-17] |
| I9 | test goldens (tiny-world, eval tie, fixtures) | `scripts/tests/fixtures`, `atx-engine/tests/fixtures/**` | T1 / lanes | tests | pinned by tests; out of the store's scope [j] |

### 1.2 Run records (outputs of the driver)

| # | class (schema) | where, count, size [m] | writer | readers | covered by |
|---|---|---|---|---|---|
| R1 | bounded run receipt `atx.bounded-research-run/v1` (`receipt.json`) | every run dir; 653 files, 1.8 MB; CRLF | `run_bounded_research.py` [c rbr@e1:423-434, 497-499] | cycle phase rows [c cycle_verdict@e1:53-60]; resume argv / exe checks [c cycle_resume@e1:117, 139]; wave timings [c wave_stage_util@e1:101-104]; `research_gc` protects it [c research_gc.py:34] | fields copied into chained stage receipts and wave-result timings (`executable_sha256`, argv), not its bytes [j from c wave_result@e1:16] |
| R2 | launch receipt `start.json` (same schema, at launch) | 654 files, 1.5 MB; CRLF | [c rbr@e1:434] | no code reader in the driver (only `research_gc` PROTECTED) [c research_gc.py:34] | none |
| R3 | stage receipt `atx.stage-receipt/v1` | `build-equity/waves/*/receipts/NN-<stage>[.failed-k].json` 37; LF, sorted keys | `stage_chain.Chain.write` [c stage_chain@e1:265-274] | chain resume (done / stale) [c stage_chain@e1:1-30]; retry detection [c wave_context@e1:239]; `wave_result.build` [c wave_result@e1:100] | **chained**: each receipt carries the previous receipt's file or content SHA-256 [c stage_chain@e1:26-30]; SHA-256s in `wave-result.receipts{}` |
| R4 | NAV cycle binding `atx.cycle-nav-binding/v1` | `<run dir>/cycle_binding.json` 50, 16 KB; CRLF | `cycle_resume.record` [c cycle_resume@e1:189] | `cycle_resume` check before a done NAV is scored [c cycle_resume@e1:1-30] | holds pins (spec digest, argv SHA); not itself pinned |
| R5 | cycle verdict `atx.cycle-verdict/v1` (+ `verdicts/<mode>-k.json` copies) | `cycle-*/cycle_verdict.json` 40, 0.34 MB; LF; holds statistics | `cycle_verdict.write_verdict` / `keep_copy` [c cycle_verdict@e1:126-147] | wave judge stage, reports | copies exist so a receipt that pins a verdict never dangles [c cycle_verdict@e1:15-17]; records ledger head |
| R6 | wave result `atx.wave-result/v1` + `wave-log.md` | `build-equity/waves/*/wave-result.json` 4; LF; holds statistics | record stage [c wave_stage_record@e1:230-231]; copied to the sprint dir [c :240-241] | `wave_scoreboard` (lineage, checks, timings) [c wave_scoreboard@e1:43, 81, 108]; `wave_queue emit --after` [c wave_queue@e1:28] | git copy in the sprint dir; records manifest SHA, ledger head, receipts{} |
| R7 | wave reader outputs, bundle | `waves/*/readers/*.json`, `bundle.json` 5 | `wave_readers.py` [c wave_readers@e1:154]; `nav_summ --bundle-json` [c nav_summ.py:1199] | record stage | digests in stage outputs (E1 content digests) |
| R8 | trial ledger `atx.trial-ledger/v1` | `build-equity/trials.jsonl` 1 file, 133 lines, 163 KB; LF; compact sorted keys | `backtest_integrity.ledger_append` under an O_EXCL lock [c bi@e1:1087-1167]; called by `nav_summ.py:951`, `holdout_gate.py:282`, `research_ledger` protocol / defect / campaign lines [c research_ledger.py:18-45] | `research_ledger.cells`, `ledger_n`, `nav_summ --dsr-ledger`, `dsr_total`, scoreboard checks, `research_cycle` `ledger+1` | **hash chain** (`prev_sha256`, legacy fold) [c bi@e1:1017-1084]; head recorded by every verdict, wave result and the committed sprint copy [c bi@e1:39-43; research_cycle@e1:2097-2111] |
| R9 | C++ IC trial ledgers + sidecar | `atx-engine/reviews/trial-ledger*.jsonl` 9 + `.manifest.json` 9 (~1.3 MB) | `trial_ledger.cpp` (stage_equity_ic) [c trial_ledger.hpp:1-20] | `verify_trial_ledger` | chain + required sidecar head [c trial_ledger.hpp:11-20]; B2 moves it to `research/ledger` |
| R10 | build receipt (`mega-<Tag>-receipt.json`) | 27, 49 KB | `scripts/research-build.ps1` [c research-build.ps1:12-20] | root, integration log | exe SHA-256s it lists back the specs' `exes_sha256` pins (`lock --exes`) [c cycle_resume@e1:20-26] |
| R11 | scoreboard md / json `atx.book-scoreboard/v1` | on demand | [c wave_scoreboard@e1:237-239] | humans | none |

### 1.3 Research outputs (exes and Python tools)

| # | class | count, size [m] | writer | pinned by |
|---|---|---|---|---|
| O1 | fields manifest `atx.research-role-fields/v1` + `*.f64` payloads | 18 manifests in fields dirs (84 `manifest.json` overall, 10.3 MB); `train-2020-2023-lo3-fields-v15` = 85 files, 5.3 GB | Python builder publish-last [c prepare_research_fields.py:99, 620]; A2 C++ manifest [c manifest.hpp@a2:1-24] | specs `fields.manifest_sha256`, wave manifests `fields.manifest_sha256` [m key structure]; payload SHA-256 per entry |
| O2 | role manifest `atx.recent-research-role/v1` (+ projection cache `atx.recent-research-projection/v1`) | role dirs | `prepare_recent_research.py:549-562, 344-358` | specs `inputs.role.sha256`; exes verify [c strategy_exposures_verb.cpp:52-60] |
| O3 | NAV `summary.json`, `recipe.json`, `v7_extras.json`, daily / events CSV, `capacity_curve.csv` | summary 346 / 62 MB (all exes); recipe 345 / 4.2 MB; extras 70 / 0.38 MB; CSV 2,320 / 1.83 GB | [c strategy_nav_replay.cpp:2707, 2746; strategy_nav_v7.cpp:430] (C1, C3) | `reference_cell`, `reference_daily` pins; ledger `trial_id` from the daily CSV [c research_ledger.py:13-17] |
| O4 | IC outputs: `orientations.json`, `recipe.json`, `summary.json` (with the `candidate_cache` block), `<role>_candidates.jsonl`, `<role>_combined.json` (`train_combined.json`), `frozen_train_receipt.json`, `marginal_ic.json` (with the `pair_cache` block) | orientations 96 / 33 MB; candidates.jsonl 110 / 19 MB; combined 84; marginal 11 | [c strategy_ic_runner.cpp:389, 583-596, 733, 752-786; strategy_marginal_ic.cpp:684] | `reference_orientations`, `reference_combined`, `reference_daily_ic` pins |
| O5 | fitter outputs `composition_weights.json` (`atx.dsl-composition-weights/v1|v2`), `admission.json/.csv` | 51 / 3.6 MB; 49 / 2.4 MB | `fit_composition_weights.publish_directory` [c fit_composition_weights.py:1782, 332] | `reference_weights`, `reference_admission` pins; theme-resid parent pins [c :470-486] |
| O6 | `nav_summ` outputs `summ.json`, `pbo.json` | 23 / 19.5 MB; 23 / 7.7 MB | [c nav_summ.py:999, 1193] (text mode: CRLF on Windows) | read into the verdict |
| O7 | report cards `card-*.json`, monitor `monitor.json`, `index.json`, diagnostics | 1,392 / 21 MB; 17 / 1.3 MB; 25 / 0.7 MB | `alpha_report_card.py`, `book_monitor.py`, `book_diagnostics.py:1558` | card / monitor manifests |
| O8 | factor series (K-P9-4a) `factor.f64`, `tau.f64`, `factor_h21.f64`, `manifest.json` | new in wave 1 | `strategy_factors_verb.cpp@b1` | D2 reads |
| O9 | other pipeline exes (`stage_equity_*`: request / seal / failure / manifest; `strategy_live` decision; mine campaign / ledger_line) | few | [c stage_equity_ic.cpp:630-2268; strategy_mine.cpp:722-724] | mine campaign line in the ledger [c research_ledger.py:36-45] |

### 1.4 Caches and payloads

| # | class | count, size [m] | writer | notes |
|---|---|---|---|---|
| C1 | record store `atx.record-store/v1` (fitter `factor`, `aim`, `aim-era`; card `card`) | `fit-work/` 384 JSON / 272 MB (~0.7 MB each); `mega-fit-work*` 122+ / 2.5 MB | `record_store.RecordStore.put` (tmp + fsync + rename) [c record_store.py:57-78]; used by [c fit_composition_weights.py:1293, 1349; alpha_report_card.py:743] | content-keyed, verified on read, "never changes an output byte" [c record_store.py:1-13]; consumers record no cache state (finding 0.5) |
| C2 | IC candidate-signal cache: `<id>.<dsl16>.f64` payload + `.json` sidecar `atx.dsl-candidate-signal/v2` | `mega-candidate-cache-v8-lo1` 48 f64 + 96 JSON, 3.0 GB; `-lo3` 117 f64 + 234 JSON, 7.3 GB | payload then sidecar published last [c strategy_ic_signal_cache.cpp:398-474]; layout [c :96-125] | sidecar is the commit marker; the u / w summary records layout and hit counts (finding 0.5) |
| C3 | IC-result cache `atx.dsl-candidate-ic/v1` | `<entry dir>/ic<v>_<key16>/<id>.json` (93 such dirs in lo3) | [c strategy_ic_result_cache.cpp:56-75, 279] | self-hashed, f64 as IEEE bit hex [c :63-69]; summary records its hits |
| C4 | marginal pair cache (S1) | `DIR/pairs<v>_<key16>/<record32>.pairs.json` shards; none on disk yet | [c strategy_marginal_pair_cache.hpp@s1:12-27] | reader loads every shard of a key; <= 2^18 pairs per key [c :32-35]; `marginal_ic.json` records shards read and hits |
| C5 | fields reuse (A2) | copies payloads from a prior fields dir | [c reuse.hpp@a2:1-30] | copy, not content-addressed (5.3 GB per version) |
| P1 | numerical payloads `*.f64`, `*.f32`, `*.u8`, `*.u64`, `*.i64`, parquet | 988 f64 = 62.0 GB (~63.5 MB each in v15); 4 f32 1.4 GB; 113 u8 0.83 GB; 92 parquet 0.25 GB | builders, IC exe | always files; SHA-256 pinned by their manifests |

---

## 2. (b) Python scripts by role

Lines are non-blank lines at the v8 head [m]. "Fate" names the P9 owner that moves or retires the module.

| role | modules | P9 owner and fate |
|---|---|---|
| orchestration: cycle | `research_cycle.py` 1,806; `research_spec.py` 288; `research_add_alpha.py` 397; `cycle_admission.py` 75; `cycle_resume.py` 94; `cycle_verdict.py` 113; `research_tree.py` 81 | E1 (w1) -> E2 (w2: move-only split into `scripts/cycle/`, `research_common.py`); stays Python in P9 |
| orchestration: wave | `research_wave.py` 72; `wave_context` 202, `wave_manifest` 270, `wave_queue` 302, `wave_readers` 136, `wave_result` 187, `wave_scoreboard` 180, `wave_seal` 110, `wave_stages` 50, `wave_steps` 190, `wave_stage_{cell,library,preflight,record,util}` 127/183/163/202/77; `wave_rules.py` 140 | E1 -> E2; `wave_rules.py:110-119` deleted by C2, `:50-51` by E2 (G-P5) |
| orchestration: runners | `run_bounded_research.py` 177; `atx-engine/tools/stage_chain.py` 213 | E1 -> E2; SQL3 adds the receipt `store` block and the store hook in wave 3 |
| orchestration: other | `research_roles.py` 243; `research_mine.py` 689 (dormant, OD-P9-9); `research_gc.py` 132; `research_ledger.py` 294 | roles / mine / gc: **no owner**; ledger: B2 |
| statistics | `nav_summ.py` 1,069; `backtest_integrity.py` 1,141; `dsr_total.py` 122; `horizon_stats.py` 46; `holdout_gate.py` 270; `compare_window_overlap.py` 529; `era_data_audit.py` 263; `mine_overlap_factor.py` 256 | B2 moves statistics of record to `atx-research-eval` and deletes the Python copies after identity; `horizon_stats.py` is on the G-P5 list but **no lane is named to delete it**; the rest **no owner** |
| composition / fit | `fit_composition_weights.py` 2,403; `composition_{ic_shrink,resid,rules,theme_erc,theme_tsmom,two_speed}.py` 1,357 | D1 (w1) -> D2 (w2: C++ fit verbs, `composition_*` deleted) -> D3 (w3) |
| field builders | `prepare_research_fields.py` 2,975 + shims; `research_fields_*.py` (13 modules, ~6,800) | A1 (registry, w1), A2 (C++ kinds), A3 (price / ohlc, w2), A4 (SEC / holdings, w3 stretch); the rest P10 |
| data / role builders | `prepare_recent_research.py` 935 (role builder); `prepare_identity_bridge` 538; `prepare_tickerhistory` 357; `build_fundamental_events` 1,379; `export_fundamental_fields` 947; `pit_fundamental_clock` 286; `era_pool` 149; `repair_role_factor_breaks` 585 | role builder P10 (migration slice 9); factor breaks A3; the rest **no owner** |
| reports / readers | `alpha_report_card.py` 1,090; `book_monitor.py` 517; `book_diagnostics.py` 1,370; `mega_report/*` | **no owner** |
| validators / infra | `code_fingerprint.py` 159 (A1); `record_store.py` 63 (SQL1 by SQL-4); `research_window.py` 103; `audit_recent_price_gap.py` 262; `audit_tickerhistory_reconciliation.py` 509 | as listed |
| generators | `atx-impl/strategies/generate_library.py` 472, `generate_from_spec.py` 160; class-C `generate_fund_ic_v*` | class-C deleted by T1; the two live generators **no owner** |

Which wave-2 / wave-3 lanes move work to C++: A2 / A3 / A4 (field builders), B1 / B2 (admission, statistics, ledger
library), C2 / C3 (gm rule, NavSpec), D2 / D3 (fit). E2 stays Python (a split). **Left with no owner after P9:**
`research_roles`, `research_mine`, `research_gc`, `alpha_report_card`, `book_monitor`, `book_diagnostics`,
`holdout_gate`, `horizon_stats` (deletion), `compare_window_overlap`, `era_data_audit`, `mine_overlap_factor`, the role
and data builders, the audits, `generate_library` / `generate_from_spec` [j]. The SQL lanes add no Python generator
(SQL-5) and move no statistic; they take `record_store.py` (wave 2) and the record writers' store hooks (wave 3).

---

## 3. (c) Target architecture

### 3.1 Layering

| layer | what | why there |
|---|---|---|
| `atx-core` `db/` (exists; SQL1 extends it in place and with new files) | the generic wrapper: RAII, `Result`, an open-with-policy function (WAL, pragmas, `application_id`, `user_version`, defensive mode, busy timeout, network-path refusal), checkpoint, quick check, checked column reads; the defects of §3.2 fixed | the only layer allowed to include `<sqlite3.h>`: `atx_sqlite3` is PRIVATE, its include dir does not propagate [c atx-core/CMakeLists.txt:1-7]; one SQLite copy per process [r SQL-4]; domain-free, so atx-impl caches use it without engine research libs [j] |
| `atx-engine` `research/store/` (library `atx-engine-research-store`, SQL1) | the compile-time table descriptor and its templates (DDL, bind, read, digest, schema JSON), the row types and table descriptors of the core and cache groups, store open / create / migrate, catalog digest | research-artifact domain; peer of `research/fields`, `research/admission`, B2's `research/ledger` [j]; the generator is C++ and compile-time [r SQL-5] |
| `atx-engine` `research/store/catalog/` (library `atx-engine-research-catalog` + exe `atx-research-store`, SQL2) | the catalog group's rows and descriptors, class registry, seal guard, ingesters, pins, CLI including `schema --json` | same domain; separate library so cache users (IC exe) never link the ingesters [j] |
| `atx-impl` (clients) | IC signal / IC-result / pair caches use the cache tables (wave 3b) | strategy exes are clients [j] |
| Python (while orchestration is Python) | one generic accessor driven by the printed schema (no generated code) [r SQL-5]; the record-store backend; the render-identity checker | reads / writes rows of stores that C++ created |

Not `atx-tsdb` and not `atx/engine/store/` (finding 0.2) [j].

### 3.2 The existing wrapper: reused, extended, fixed in place (SQL1, ruling SQL-4)

Reused: RAII handles, `Result` returns, `Transaction::begin_immediate` [c sqlite.hpp:240-244], `prepare_cached`, online
backup [c sqlite.cpp:374-418]. Defects SQL1 fixes in place [r SQL-4]:

| # | defect | where | fix |
|---|---|---|---|
| W1 | `prepare` ignores the SQL tail, so a multi-statement string runs its first statement only | [c atx-core/src/db/sqlite.cpp:325-333] | pass `pzTail`; refuse (`ParseError`) a tail that holds anything but whitespace or comments |
| W2 | `prepare` casts `sql.size()` to `int` unchecked | [c sqlite.cpp:328] | `OutOfRange` above `INT_MAX`, as `bind` already does [c sqlite.cpp:133-135] |
| W3 | every `SQLITE_CONSTRAINT` maps to `AlreadyExists`; only the primary code is seen | [c sqlite.cpp:30-38, 68-75] | enable extended result codes at open; UNIQUE / PRIMARY KEY -> `AlreadyExists`, CHECK / NOT NULL / FOREIGN KEY / TRIGGER -> `InvalidArgument` |
| W4 | column readers return 0 / empty for NULL or a wrong storage class, unchecked | [c sqlite.hpp:135-143; sqlite.cpp:212-260] | keep them (callers exist) and add checked readers `column_checked<T>` returning `Result<T>` (refuse NULL for non-optional, wrong storage class) |
| W5 | `BlobStream::read/write` narrow `i64` offset and `size_t` length to `int` unchecked | [c sqlite.cpp:506-518] | range-check offset and length against `size()` and `INT_MAX` first |
| W6 | `Transaction` destructor runs `exec("ROLLBACK")`, which allocates a `std::string`, inside `noexcept` | [c sqlite.cpp:434-440, 319-322] | a non-allocating rollback path (`sqlite3_exec` on a string literal) |
| W7 | `Database::open` sets no busy timeout, no defensive config, no extended codes | [c sqlite.cpp:279-289] | the policy open (§3.3) in a new `connection.{hpp,cpp}`; `open` itself unchanged for existing callers |
| W8 | `prepare_cached` discards the `reset()` / `clear_bindings()` status | [c sqlite.cpp:338-342] | documented as intentional (a stale prior-step code); kept, the comment states why |

Identity of W1-W8: the 23 existing `Db*` gtests and the engine `store` / `library` groups (the existing users) pass
unchanged; no research exe calls this code [j].

### 3.3 Connection policy (Windows, concurrent launches)

| setting | catalog | cache index | why |
|---|---|---|---|
| file | `build-equity/research-store/catalog.sqlite` (one per build tree; git-ignored by `build-*/` [c .gitignore:12]) | `<cache root>/index.sqlite` for every cache (one name, one cache schema; the record store looks in its root and that root's parent) | catalog is derived and rebuildable; caches are deletable with their dir [j] |
| `application_id` | `0x41545843` ("ATXC") | `0x4154584B` ("ATXK") | a foreign or swapped file is refused at open [w https://sqlite.org/pragma.html#pragma_application_id] |
| `user_version` | schema version (1) | schema version (1) | migrations (§3.4) [w pragma.html#pragma_user_version] |
| journal | `WAL` (persistent once set) | `WAL` | readers never block the writer; one writer at a time [w https://sqlite.org/wal.html] |
| `synchronous` | `FULL` | `NORMAL` | in WAL a failed sync can corrupt only during a checkpoint; NORMAL may lose the last commits on power loss but stays consistent [w howtocorrupt.html §3.1] [j] |
| `page_size` | 8192, set on a new empty file before `journal_mode=WAL` and the first table (a WAL database cannot change page size without leaving WAL) | 8192 | 8-16 KiB pages are best for large values [w https://sqlite.org/intern-v-extern-blob.html] |
| `busy_timeout` | 30,000 ms | 30,000 ms | writes are short; the ledger lock already waits 60 s [c bi@e1:1087-1102] [j] |
| write txn | `BEGIN IMMEDIATE`, bounded retry (3 x 50 ms) on `SQLITE_BUSY` after the handler gives up | same | no read-then-upgrade deadlock [c sqlite.hpp:240-244] |
| `foreign_keys` | ON (C default already ON [c atx-core/CMakeLists.txt:14]; Python default OFF, so the Python accessor sets it) | ON | |
| hardening | `trusted_schema=OFF`, `SQLITE_DBCONFIG_DEFENSIVE=1`, extended result codes ON | same | a crafted schema cannot run functions; `writable_schema` blocked [j] |
| path | refused when the resolved path starts with `\\` (UNC) or is on a network drive | same | WAL needs shared memory on one host [w wal.html] |
| readers | no read transaction spans a computation; read, copy out, end | same | a long reader starves checkpoints and grows the WAL [w wal.html] |
| checkpoint | `wal_checkpoint(TRUNCATE)` at the end of each catalog run | at cache close (best effort) | bounded `-wal` size [j] |
| threads | one connection per thread (`THREADSAFE=2`) [c sqlite.hpp:22-27]; or one connection under a `std::mutex` never held across hashing or file I/O | same (IC exe workers) | multi-thread mode forbids concurrent use of one connection [j] |
| never | delete `-wal` / `-shm` beside a DB; put a DB under OneDrive; open a DB from two SQLite copies | | a hot WAL removed after a crash corrupts [w howtocorrupt.html §1.3] |

### 3.4 Schema versioning and migrations (C++ only)

- Version = `PRAGMA user_version`; identity = `PRAGMA application_id`; `store_info` holds the schema JSON the store
  printed at creation / migration (§3.6), so every store describes itself [j].
- Open: `application_id == 0` and the file empty -> create (`page_size`, WAL, every table's DDL, stamp both pragmas) in
  one `BEGIN IMMEDIATE`; `application_id` mismatch -> refuse (`ErrorCode::InvalidArgument`, CLI exit 3);
  `user_version > known` -> refuse (newer code wrote it; `ErrorCode::NotImplemented`, no new enumerator
  [c atx-core/include/atx/core/error.hpp:29-47]); `user_version < known` -> apply the steps `u+1 .. known` in one
  transaction (both pragmas are transactional) [j].
- Steps come from the descriptors: a table has `since`, a column has `since`; a step creates the tables with
  `since == v` and `ALTER TABLE ADD COLUMN` for columns with `since == v` (such a column must be `std::optional`, a
  `static_assert` enforces it). Anything else needs a hand-written step function registered by version. Only C++
  creates or migrates; Python refuses a store whose `user_version` differs from its schema JSON. Catalog and caches
  are derived, so `catalog --rebuild` / deleting a cache is always a fallback [j].

### 3.5 Determinism

- **Natural keys only**, no surrogate ids: every table is `WITHOUT ROWID` with its natural primary key
  [w https://sqlite.org/withoutrowid.html], so content does not depend on insertion order [j].
- **Every read that feeds an output, a digest or a report has `ORDER BY` the primary key** (the template-built
  `select_all` always does); unordered results are undefined [w https://sqlite.org/lang_select.html]; tests run with
  `PRAGMA reverse_unordered_selects=ON` [w pragma.html#pragma_reverse_unordered_selects] [j].
- **Volatile columns** (wall clock, seconds, RSS samples, pids, catalog run ids) carry the descriptor flag
  `volatile`: stored, never digested; a volatile table (run bookkeeping) is left out of the catalog digest; E1 already
  excludes time keys from chain digests [c stage_chain@e1:26-30] [j].
- **Record digest `atx.record-digest/v1`**, produced by the descriptor templates [r SQL-5]: SHA-256 over
  `"atx.record-digest/v1\n"`, then `"row <table>@<version>\n"`, then per non-volatile column in declared order
  `<name>=<value>\n` with value `~` (NULL), `i<decimal>` (int), `b0|b1` (bool), `r<16 hex of IEEE-754 bits>` (real,
  no NaN canonicalisation), `u<16 hex>` (u64), `t<byte length>:<UTF-8 bytes>` (text, sha256, relpath, json),
  `x<length>:<bytes>` (blob). A logical record with children (a run and its argv rows) appends each child row's
  encoding in key order. Reals as bit hex follow the IC-result cache precedent [c strategy_ic_result_cache.cpp:63-69].
  Python computes no digest (no second implementation); the gtest golden vectors come from a test-only oracle script
  in the fixture directory (the T1 `eval_tie` precedent) [j].
- **Catalog digest `atx.catalog-digest/v1`**: SHA-256 over `"atx.catalog-digest/v1\n"` + `<table> <row digest>\n`
  for every non-volatile table in descriptor order and every row in key order. Reproducibility compares this digest,
  never the `.sqlite` bytes [j].
- **JSON columns**: each table has one writer language in P9 (catalog: C++ ingest, nlohmann `ordered_json` compact
  dump, insertion order kept for rendering; `record`: Python compact dump in insertion order as `record_store`
  requires [c record_store.py:57-62]). A JSON column's digest covers that writer's text; RFC 8785 canonical JSON is
  P10, needed only when a table gets a second writer [j].
- **Typed columns never hold a return, IC, Sharpe, turnover, drawdown, DSR / PSR / PBO or NAV value**: those stay
  inside `doc` JSON, so catalog queries are blind-safe by construction; the real catalog file is root-only [r SQL-7]
  [j].

### 3.6 Compile-time table descriptors (ruling SQL-5)

One `constexpr` descriptor per record type, written beside its row struct; templates produce from it the DDL string,
the bind code, the read code, the record digest and the schema JSON. No generated source file, no host code-gen step,
no Python generator [r SQL-5].

**Row and descriptor.** A row is a plain aggregate; nullability is the member type (`std::optional<T>` = NULL
allowed, anything else = `NOT NULL`), so an illegal NULL is unrepresentable:

```cpp
struct RunRow {
  std::string run_dir;                       // key
  std::optional<std::string> argv_sha256;
  std::optional<atx::i64> attempt;
  std::optional<atx::f64> wall_seconds;      // volatile
  // ...
};

inline constexpr auto kRunTable = store::table<RunRow>(
    "run", store::TableOpts{.version = 1, .since = 1},
    store::col<store::Sql::RelPath>("run_dir", &RunRow::run_dir, store::kKey),
    store::col<store::Sql::Sha256>("argv_sha256", &RunRow::argv_sha256, store::kIndexed),
    store::col<store::Sql::Int>("attempt", &RunRow::attempt),
    store::col<store::Sql::Real>("wall_seconds", &RunRow::wall_seconds, store::kVolatile));
```

- `enum class Sql : u8 {Int, Bool, Real, U64, Text, Sha256, RelPath, Json, Blob}`; `col<Sql T>(name, M Row::*, flags,
  allowed = {})` is `consteval` and `static_assert`s, with a message, that `M` (or `M::value_type` for an optional)
  matches `T`: `Int` - `atx::i64`; `Bool` - `bool`; `Real` - `atx::f64`; `U64` - `atx::u64` (stored as its bit
  pattern via `std::bit_cast<atx::i64>`); `Text` / `Sha256` / `RelPath` / `Json` - `std::string`; `Blob` -
  `std::vector<std::byte>`. Concepts constrain the member types (`.agents/cpp/agent.md` §6).
- Flags: `kKey` (primary-key member; the key is the flagged columns in declared order), `kIndexed` (one single-column
  index), `kVolatile`; `allowed` is a `std::span<const std::string_view>` over a namespace-scope `constexpr` array
  (a `CHECK(x IN (...))`). Table options: `version`, `since`, `append_only` (emits `BEFORE UPDATE` / `BEFORE DELETE`
  triggers with `RAISE(ABORT, ...)` [w https://sqlite.org/lang_createtrigger.html]), `volatile_table`. A column added
  later carries `since`; `consteval` checks: at least one key column, unique names, at most 32 columns, a later column
  is optional.
- Type-derived CHECKs: `Bool` `IN (0,1)`; `Sha256` `length(x)=64 AND x NOT GLOB '*[^0-9a-f]*'`; `RelPath` no `\`, no
  leading `/`, no `..` segment; `Json` `json_valid(x)`.
- **Exact DDL grammar** (so the Python test fixture can be written by hand and checked by root): `CREATE TABLE <t>(`
  then columns joined by `", "`, each `<name> <INTEGER|REAL|TEXT|BLOB>[ NOT NULL][ CHECK(<expr>)]`, then
  `, PRIMARY KEY(<k1>, <k2>)`, then `) STRICT, WITHOUT ROWID;`; then `CREATE INDEX ix_<t>_<c> ON <t>(<c>);` per
  indexed column in declared order; then the two triggers for an append-only table. Views (`pin_status`) are plain
  `std::string_view` SQL constants registered with the DB, not descriptors.

**Operations** (function templates over a descriptor, in a private header): `ddl(table) -> std::vector<std::string>`;
`insert_sql`, `upsert_sql` (`INSERT ... ON CONFLICT(<key>) DO UPDATE`), `select_all_sql` (`ORDER BY` the key);
`bind(Statement&, table, const Row&) -> Status` and `read(const Statement&, table) -> Result<Row>` as fold
expressions over `std::index_sequence` (no recursion, no SFINAE); `encode(DigestStream&, table, const Row&)`;
`table_schema(table) -> TableSchema` (a plain struct; no JSON type in any header). Each group's tables are folded
into one `GroupOps` (a plain struct of function pointers: `tables()`, `steps(to_version)`, `digest(db, stream)`,
`views()`), so the non-template `open_store(path, DbKind, groups)`, `schema_json(DbKind, groups)` and
`catalog_digest(db, groups)` combine groups without knowing them: the catalog DB is `{core_group(),
records_group()}`, a cache DB `{cache_group()}`. SQL1 never names SQL2's group.

**Compile-time cost and how it is bounded** (the ruled cost of SQL-5) [j]:
- Three header tiers. `rows_<group>.hpp` (public): plain row structs only, no templates; every consumer (ingesters,
  IC caches, CLI, tests) includes only these, `ops_<group>.hpp` and `store.hpp`. `ops_<group>.hpp` (public):
  non-template declarations per table, e.g. `insert(Database&, const RunRow&)`, `upsert(...)`,
  `read_run(const Statement&)`, `select_all_run(Database&)`, `digest(const RunRow&)`, and the group's `GroupOps`
  accessor. `detail/table.hpp` + `detail/table_ops.hpp` + `tables_<group>.hpp` (descriptors) live under
  `src/research/store/` and are included **only** by the instantiating TUs.
- Exactly one instantiating TU per group: `src/research/store/core_ops.cpp` and `cache_ops.cpp` (SQL1),
  `src/research/store/catalog/records_ops.cpp` (SQL2), plus one gtest TU that instantiates a toy table. Editing a
  group's descriptors recompiles one TU; consumers never instantiate the templates.
- Neither tier enters `pch.hpp` (`.agents/cpp/agent.md` §8: no volatile headers in the PCH).
- Instantiation count is linear: ~30 tables x 6 operations, each a fold over <= 32 columns; DDL is built at runtime
  with `std::string` once per open (no `constexpr` string building). Expected cost: seconds per instantiating TU
  [est, not measured; root logs the three TUs' compile times in the merge build log].

**Python: one generic accessor, no generated code** [r SQL-5]. `atx-research-store schema --json --db catalog|cache`
prints `atx.store-schema/v1`: `{schema, db, application_id, user_version, page_size, groups: [{name, db, version,
tables: [{name, version, since, append_only, volatile, key: [...], columns: [{name, type, nullable, indexed,
volatile, since, allowed}], ddl: [...]}], views: [{name, sql}]}]}` (nlohmann `ordered_json`, keys in this order,
two-space indent, LF, final newline). The same text is stored in `store_info('schema_json')` when the store is created
or migrated, and `atx-engine/tools/research_store.py` reads it from the store it opens (one generic `Store` with
`insert`, `upsert`, `get`, `select` ordered by key, type checks per column, the policy pragmas); it never runs DDL or
migrations, never computes a digest, and refuses a store whose `user_version` or `application_id` differs from the
schema it carries. Tests use one committed fixture per group, `tests/fixtures/research_store/schema/<group>.json`
(the group object in the printed form, written by hand from the grammar; per group so no file has two owners), and
build temporary stores with the test-only `make_store.py`, which executes the fixtures' `ddl` lists.

**Drift guards** [j]: (1) gtest `ResearchStoreSchema.GroupJsonEqualsFixture` (and SQL2's
`ResearchCatalog.RecordsSchemaJsonEqualsFixture`) compares each group's printed object with its fixture byte for byte
(the fixtures Python's tests use); (2) gtest `.DdlAppliesOnVendoredSqlite`; (3)
pytest applies every fixture `ddl` statement on Python's SQLite 3.43.1; (4) gtest `ResearchStoreDigest.GoldenVectors`
against oracle vectors; (5) `consteval` checks in every descriptor; (6) a gtest reads a store created by the
test-only Python fixture builder (cross-version file compatibility).

### 3.7 Schema v1 (K-P9-13)

`?` = `std::optional` member (nullable); `~` = `kVolatile` (stored, never digested); **bold** = `kKey`; `[ix]` =
`kIndexed`. All tables STRICT, WITHOUT ROWID.

**Catalog DB, group `catalog_core` (SQL1):**
- `store_info(`**key** text, value text`)`: `db_kind`, `schema_json`, `created_by` (exe SHA-256).
- `catalog_run` (volatile table) `(`**catalog_run_id** text, git_sha text?, store_exe_sha256 sha256?, roots json,
  started_utc text, seconds real, files_seen int, files_verified int, files_declared int, files_skipped int,
  catalog_digest sha256?`)`.
- `artifact(`**path_key** text (case-folded, `/`), path relpath, sha256 sha256 [ix], bytes int?, class text [ix],
  json_schema text?, sha_source text allowed {`verified`, `declared`}, declared_by relpath?, eol text? allowed {`lf`,
  `crlf`, `mixed`, `none`}, producer_key sha256?`)`: the current tree state. `verified` = the catalog hashed the
  bytes itself; `declared` = the SHA-256 a manifest or receipt states for a file the catalog did not open (sealed or
  over 16 MiB) [r SQL-7].
- `artifact_seen` (volatile, append-only) `(`**path_key, sha256, catalog_run_id**`)`: history across catalog runs.
- `skipped_path` (volatile) `(`**catalog_run_id, path** text, reason text allowed {`seal-name`, `outside-roots`,
  `declared-only`, `unreadable`, `unparsed`}`)`.
- `producer(`**producer_key** sha256 (record digest of the other columns), kind text allowed {`engine`, `python`,
  `powershell`, `human`, `unknown`}, exe_sha256 sha256?, git_sha text?, build_type text?, module text?, code_sha256
  sha256?, receipt_sha256 sha256?`)`: K-P9-3 and ruling P6 shapes.

**Catalog DB, group `catalog_records` (SQL2):**
- `run(`**run_dir** relpath, receipt_schema text, source_sha text?, executable_sha256 sha256? [ix], argv_sha256
  sha256? [ix], attempt int?, build_type text?, role_id text?, outcome text [ix], exit_code int?, git_state text?,
  error text?, limits json?, admission json?~, dirty_outside json?, store json?, started_utc text?~, wall_seconds
  real?~, sampled_peak_tree_rss_bytes int?~, minimum_system_free_bytes int?~, owned_processes json?~,
  ownership_scope text?, logs json?, key_order json, extra json?, file_sha256 sha256`)`; children
  `run_command(`**run_dir, ord**, arg text`)`, `run_binding(`**run_dir, ord**, path text, sha256 sha256`)`. Render
  mode `typed`. (`store` is SQL3's receipt block, §3.10.)
- `run_start(`**run_dir** relpath, file_sha256, doc json`)`: render mode `doc`. Every renderer takes the line end from
  the file's `artifact.eol`.
- `stage_receipt(`**state_dir** relpath, **file_name** text, stage_index int, stage text [ix], status text, schema
  text, chain text, inputs json, outputs json, started_utc text~, seconds real~, extra json?, file_sha256`)`. Render
  `typed`, rule `py-indent2-sorted` (sorted keys, so no key order is stored).
- `cycle_binding(`**run_dir** relpath, schema text, output relpath, spec_sha256 sha256, spec_rule text?, argv_sha256
  sha256?, key_order json, extra json?, file_sha256`)`. Render `typed`.
- `cycle_verdict(`**path** relpath, schema text, cycle text, mode text, spec_sha256 sha256?, ledger_path relpath?,
  ledger_head sha256?, ledger_lines int?, doc json, file_sha256`)`. Render `doc`.
- `wave_result(`**path** relpath, schema text, wave text, kind text, manifest_path relpath, manifest_sha256 sha256,
  accepted bool?, ledger_head sha256?, n_before int?, n_after int?, trial_id text?, next_parent_spec relpath?, doc
  json, file_sha256`)` + `wave_timing(`**path, ord**, phase text, run_dir relpath?, outcome text?, exit_code int?,
  seconds real?~, peak_mib int?~, executable_sha256 sha256?`)` (query extract). Render `doc`.
- `spec_doc(`**path** relpath, kind text allowed {`spec`, `template`, `wave-manifest`, `candidate`, `library`,
  `recipe`, `registry`, `window`, `field-registry`, `other`}, schema text?, file_sha256, git_tracked bool, parent
  text?, doc json`)`.
- `pin(`**holder_path** relpath, **pointer** text (RFC 6901 pointer into the holder), pin_kind text allowed
  {`input`, `locked`, `fields-manifest`, `rule-template`, `exe`, `binding`, `receipt-binding`, `receipt-log`,
  `spec-digest`, `ledger-head`, `wave-manifest`, `stage-receipt`, `field-payload`, `field-registry`,
  `field-source`}, target_path text? [ix], target_sha256 sha256 [ix]`)`; view `pin_status` = pin x `artifact` ->
  `ok` (target verified and equal), `declared` (target only declared, equal), `stale`, `missing`, `unresolved`
  (digest pins, outside-root targets) [r SQL-7].
- `trial_line` (append-only) `(`**ledger_path** relpath, **seq** int, line text (exact bytes, no line end),
  line_sha256 sha256, schema text?, kind text?, cell text?, trial_id text?, count int?, prev_sha256 sha256?`)`.
- `ledger_state(`**ledger_path** relpath, lines int, file_sha256, head_sha256 sha256?, head_rule text?`)`: the head is
  filled only by B2's C++ chain-head function (SQL4); the catalog never re-implements the chain rule (G-P5) [r SQL-7].
- `field_manifest(`**path** relpath, schema text, status text?, role_manifest_sha256 sha256?, seal_exclusive_end
  text?, registry_sha256 sha256?, engine_exe_sha256 sha256?, file_sha256, doc json`)` + `field_entry(`**path,
  name**, ord int, file text, sha256 sha256?, dtype text?, producer_key sha256?, formula_sha256 sha256?, reused_from
  text?`)`.
- `candidate(`**id** text, path relpath, status text, wave text?, dsl_sha256 sha256?, file_sha256, doc json`)` +
  `candidate_event(`**id, ord**, status text, at text, by text, wave text?, note text?`)`. Render `doc`, rule
  `py-indent2-noascii`.
- `build_receipt(`**tag** text, path relpath, preset text?, source text?, exit_code int?, wall_seconds real?~,
  file_sha256, doc json`)` + `build_exe(`**tag, target** text, sha256 sha256 [ix]`)` (exe SHA -> tag -> git SHA).

**Cache DB, group `cache` (SQL1 defines; SQL1's record store writes `record`, SQL4 writes the rest):**
- `record(`**root** text, **kind** text, **key_sha256** sha256, key json, body json?, body_file relpath?, body_bytes
  int, content_sha256 sha256`)`; exactly one of `body` / `body_file` (checked by the writer and a table CHECK).
- `ic_signal(`**key_sha256** sha256, schema text, role_sha256 sha256, id text, dsl_sha256 sha256, vm_identity text,
  eval_mode text, payload relpath, payload_sha256 sha256, payload_bytes int, sidecar json`)`.
- `ic_result(`**entry_dir** relpath, **id** text, key_sha256 sha256, record json, record_sha256 sha256`)`.
- `pair_key(`**key_sha256** sha256, version int, key_text text`)` + `pair_stat(`**key_sha256, payload_lo,
  payload_hi** sha256, sum_bits u64, dates int`)`, writer-checked `payload_lo <= payload_hi`.

### 3.8 Payloads stay files; the inline threshold

Values up to **1 MiB** are stored inline (TEXT / BLOB); larger values are files referenced by SHA-256 and byte count
(record bodies over 1 MiB go to `<store>/objects/<sha[0:2]>/<sha>.json`, tmp + fsync + rename as today
[c record_store.py:57-78]). Every numerical payload (`*.f64` ~63.5 MB, `*.u8`, `*.f32`, parquet) stays a file
referenced by its manifest's SHA-256 and is never a BLOB. Argument [j]: values under ~100 KB read faster from the DB,
with 8-16 KiB pages best for large values [w intern-v-extern-blob.html]; on Windows 10 ~10 KB reads were ~5x faster
from SQLite and antivirus slows direct file writes by an order of magnitude [w https://sqlite.org/fasterthanfs.html];
between 100 KB and 1 MiB either is fast while the DB gives atomic publish and one-file garbage collection; 1 MiB is
the house cap for metadata files [c atx-impl/src/strategy_ic_detail.hpp:63]; payloads are 60x above it, are read by
streaming / mapped readers that a BLOB would defeat, and would make backups proportional to data. Cost if wrong: a
re-pack of a derived DB.

### 3.9 The catalog (stage 1) and the identity checker (stage 2 evidence)

- **Roots from pins (seal and blindness)** [r SQL-7]. The catalog opens only: repo-tracked inputs (I1-I8); files a
  holder it has opened pins, transitively from `scripts/specs/{v8,p9}/**`, `scripts/specs/*.json`, the wave manifests
  and the ledger; files inside the run, cycle and wave dirs those holders name; explicit `--root` additions.
  Everything else under `build-equity/` is listed by name only (`skipped_path`, `outside-roots`). Backstop: a path
  segment holding a standalone year token 2024-2099 (`(^|\D)20(2[4-9]|[3-9]\d)(\D|$)`) is never opened
  (`seal-name`; 8 top-level dirs match today [m]). `field-source` pins (builder inputs outside the TRAIN outputs) are
  recorded and never followed. Files over 16 MiB are never opened: their SHA-256 is recorded as **declared** by the
  manifest that pins them; a hash the catalog computed itself is **verified**; the two are never merged and
  `pin_status` reports `declared` separately from `ok` [r SQL-7].
- **Real-tree ingest is a root-only bounded run** (`run_bounded_research.py`, <= 600 s, <= 4,096 MiB, 0 trials); the
  catalog file holds verdict and wave-result documents and is opened only by root; lanes and reviewers use the
  synthetic fixture tree [r SQL-7].
- **Hashing** uses `atx::core::Sha256` (optimized even in Debug [c atx-core/CMakeLists.txt:80-87]); a catalog run
  verifies the ~14,000 small files (~2.2 GB) and declares the ~1,000 payloads [m] [j].
- **Ingest** parses a JSON class with `nlohmann::ordered_json` (key order kept for rendering), fills the typed columns,
  keeps undeclared keys in `extra` (in order) and the key order in `key_order`; a number outside i64 / u64 / double, a
  NaN token or invalid UTF-8 -> artifact only, `unparsed` [j].
- **Render-identity checker** (Python, the writers' language, on the generic accessor): rebuilds each stage-2 record
  from its rows and the class's render rule and compares SHA-256 with the file:

  | rule | bytes | line end as written on Windows | classes |
  |---|---|---|---|
  | `py-indent2` | `json.dumps(doc, indent=2) + "\n"` | CRLF (text mode, no `newline=`) | R1 receipt, R2 start, R4 binding [c rbr@e1:434, 497-499; cycle_resume@e1:189] |
  | `py-indent2-sorted` | `json.dumps(doc, indent=2, sort_keys=True, allow_nan=False) + "\n"` | LF | R3 stage receipt [c stage_chain@e1:273-274] |
  | `py-indent2` | `json.dumps(doc, indent=2) + "\n"` | LF (`newline="\n"`) | R5 verdict + copies [c cycle_verdict@e1:146-147], R6 wave result [c wave_context@e1:75-77], cell specs [c wave_stage_util@e1:89-97] |
  | `py-indent2-noascii` | `json.dumps(doc, indent=2, ensure_ascii=False) + "\n"` | LF | I4 candidates [c wave_queue@e1:141], emitted manifests [c :344] |
  | `py-compact-sorted-lines` | `json.dumps(rec, sort_keys=True, separators=(",", ":"))` per line + `\n` | LF | R8 ledger lines [c bi@e1:1164] |
  | `py-indent2-sorted` | `json.dumps(doc, indent=2, sort_keys=True) + "\n"` | CRLF (text mode) | O6 `summ.json`, `pbo.json` (stage 1 only) [c nav_summ.py:999, 1193] |

  The line end is read at ingest into `artifact.eol` and re-applied at render, so the check is host-independent.
  Floats render exactly: a Python `repr` text parses to one double, C++ stores it, Python renders `repr` again [j].
- **CLI** `atx-research-store` (SQL2): `init`, `catalog`, `ingest --class C --path P`, `verify --pins [--spec S]
  [--strict]`, `query {artifacts, pins, stale-pins, runs, timings, producers}`, `digest`, `dump --table T` (sorted JSON
  lines, for review diffs), `cache init DIR` (creates `DIR/index.sqlite`; the only way a cache index is created),
  `cache prune --base DIR`, `schema --json --db catalog|cache`, `quick-check`. Exit 0 ok, 2 usage, 3
  refusal (stale pin under `--strict`, seal, wrong application id), 4 error [j].
- **Class registry**: `atx-engine/schemas/research_store/classes.json` lists every class of §1 (id, path globs, JSON
  schema id, writer file, ingest table, render rule, pinned-by, stage) and is data the catalog reads at run time (a
  registry of classes, not a schema generator). A guard pytest requires every `atx.<name>/v<n>` literal written by
  Python or C++ under `scripts/`, `atx-engine/`, `atx-impl/` to be registered (dated `legacy_allow` rows for legacy
  classes) [j].

### 3.10 Cache backend selection (ruling SQL-6)

A cache may select its SQLite index by file presence only where the consumer's output bytes are identical either way
**and** the choice is written into the run receipt; otherwise an explicit flag [r SQL-6]. Per cache:

| cache | consumer bytes identical either way? | selection | where the choice is recorded |
|---|---|---|---|
| C1 record store (fitter, card) | yes: the fitter and the card write no cache state [c fit_composition_weights.py:1293-1325; alpha_report_card.py:743-755] | **file presence**: `index.sqlite` in the record root or its parent (the fitter's `--work-dir` [c fit_composition_weights.py:1822], the card's store root [c alpha_report_card.py:743]) | the bounded-run receipt's `store.indexes` block (SQL3, wave 3: the runner lists every store index file found in or beside a directory named on the command line, in `start.json` and `receipt.json`; absent when none, so today's receipts are byte-identical); until SQL3 merges, the backend also prints one stderr line naming its selection, which the receipt pins through `logs`, and real research cells do not opt in (root's identity runs only) |
| C2 IC signal sidecars, C3 IC results | no: the u / w summary records the cache layout and hit counts [c strategy_ic_runner.cpp:583-596] | **explicit flag** `--candidate-cache-index sqlite` on the IC exe; with it the summary's `candidate_cache` block gains `"index": "sqlite"`; absent flag = today's code and bytes | argv: the receipt's `command` and `argv_sha256` |
| C4 marginal pair cache | no: `marginal_ic.json` records shards read and hits [c strategy_marginal_ic.cpp@s1:1011-1014] | **explicit flag** `--pair-cache-index sqlite` on the marginal verb; with it the `pair_cache` block gains `"index": "sqlite"` and reports rows read in place of shards | argv, as above |

With an index, new entries are written to the DB only (JSON retired for that cache) and a DB miss falls back to the
legacy JSON entry, verified exactly as today, then imported (**read-through**). Payload `.f64` files stay where they
are; for the signal cache the row replaces the sidecar as the commit marker (payload published no-replace first, row
inserted second) [c strategy_ic_signal_cache.cpp:398-474] [j]. `research_gc` keeps deleting whole store dirs
[c research_gc.py:5-19]; orphaned `record` rows of a deleted fit-work root are pruned by
`atx-research-store cache prune` (root step) [j].

The wave-3 writer hook (SQL3) follows the same rule: it is enabled by the presence of the catalog file and the
built exe, it never changes a research output byte, and the bounded-run receipt records it in `store.catalog` [j].

### 3.11 The trial ledger index

The hash-chained JSONL stays the authority through P9 [r SQL-9]; SQLite holds an index of it: each line's exact bytes
and SHA-256 in an append-only table (triggers stop accidental edits only; tamper evidence remains the text chain,
whose heads are recorded in verdicts, wave results and the git-committed sprint copy [c bi@e1:39-43]). The head is
computed only by B2's C++ chain-head function (SQL4) and compared with the recorded heads; the index never decides N.
Whether JSONL stays the authority beyond P9 is the owner's P10 decision [r SQL-9].

---

## 4. (d) Migration path and stage per class

Stages [r SQL-2]: **1** = indexed by the catalog (path, SHA-256 verified or declared, typed keys, pins) or, for an
unpinned cache, moved into SQLite; **2** = dual-write (the writer ingests what it just wrote) with the render checker
proving the rows regenerate the bytes; **3** = JSON retired.

| class | stage in P9 | when / lane | evidence that allows it |
|---|---|---|---|
| C1 record store | **3** (DB only for new writes; read-through) | backend wave 2 (SQL1); real cells opt in after SQL3's receipt block (wave 3) | consumer bytes identical (finding 0.5); root: X-5 fit and card byte-identical with no index, cold and warm index |
| C2 IC signal sidecars, C3 IC results, C4 pair shards | **3** for the index (payload files stay) | wave 3b, SQL4 (explicit flags) | flag absent = today's bytes; with the flag, X-5 u / w byte-identical except the summary's `candidate_cache` block and the ruled timing-only files; marginal rows identical, `pair_cache` block aside |
| R1 receipt, R2 start receipt, R4 binding | **2** (wave 2: stage 1, render check run by root on catalogued files) | wave 3, SQL3 | catalog + checker: 0 mismatches of N; per-write check in the hook |
| R3 stage receipt | **2** | wave 3, SQL3 (`stage_chain.py` generic write callback) | same |
| R5 verdict, R6 wave result, I4 candidate | **2** | wave 3, SQL3 | same (doc mode) |
| R7 readers / bundle, O6 summ / pbo | 1 | wave 2 | writers owned by E2 / B2; P10 |
| R8 trial ledger | 1 (index; head via B2 in wave 3b) | wave 2 / 3b | JSONL authority through P9 [r SQL-9] |
| R9 C++ ledgers, R10 build receipts | 1 | wave 2 | |
| I1-I3, I5-I8 inputs | 1 (text in git, ingested) | wave 2 | see below |
| O1-O9 research outputs | 1 | wave 2 | writers owned by A-, C-, D-, S-lanes; P10 |
| P1 payloads | never in the DB | - | §3.8 |

**Stage 3 for any pinned class is P10**, after: one full wave of dual-write with 0 render mismatches; every reader of
the class reads through the store; pins re-expressed as record digests under one ruled re-pin (DEC-20 style);
`VACUUM INTO` snapshots and an `integrity_check` gate for authority classes. No earlier point moves no pinned byte:
retiring any receipt, binding, verdict or wave result changes the run-dir and wave-dir file sets the identity runs
compare (E1's fake-wave identity counts files [c progress.md:140]) and removes bytes that verdicts, scoreboard checks
or the resume checks read [j].

**Human-authored inputs stay text files in git through P9 and are ingested** (recommendation for P10 too, owner's
call) [j]: specs, templates, wave manifests, candidate registrations, libraries, registries and the research window
are reviewed in diffs, pre-registered by commit before any run, and pinned by the SHA-256 of their bytes; a DB row
cannot be reviewed in a pull request, blamed or merged, and a pin over a DB row would need a canonical form that the
file already is. The catalog's `spec_doc` + `pin` rows give the query surface without moving the authority.

---

## 5. (e) Python to C++: what moves in this workstream, and the ownership order

Moves to engine C++ in the SQL lanes:
1. Table and schema generation: compile-time descriptors and templates (DDL, bind, read, digest, schema JSON); no
   Python generator exists or is added [r SQL-5].
2. Artifact identity and indexing (hash, classify, producer, pin extraction): `atx-research-store catalog / ingest`.
   No Python equivalent exists, so no mirror is created [j].
3. Pin audit across every spec (`verify --pins`): a report over all holders; the cycle's run-time pin refusal stays in
   E-lane Python until P10 (a byte-equality check, not a G-P5 rule) [j].
4. IC cache indexes (C++ already) onto the cache tables (SQL4).
5. Trial-ledger index head through B2's C++ chain-head function (SQL4); no Python chain code is added [r SQL-7].

Stays with other lanes: statistics of record (B2), fit (D2 / D3), cycle split and manifest kinds (E2), field builders
(A2 / A3 / A4), NAV spec (C3). P10: bounded runner and stage chain in C++ over the store; scoreboard and pin checks
reading the store; `research_gc` by query; content-addressed fields reuse (hard links instead of 5.3 GB copies)
[c reuse.hpp@a2:21-23]; a C++ record-cache API (with Python-compatible canonical JSON) when a C++ fit needs it; C++
writers dual-writing their own outputs.

File ownership by wave (no file has two owners in a wave):

| files the SQL lanes touch | wave 1 | wave 2 | wave 3 / 3b |
|---|---|---|---|
| `atx-core/{include/atx/core,src}/db/sqlite.*`, `atx-core/tests/db_sqlite_test.cpp`; new `db/connection.*`, `atx-core/tests/db_connection_test.cpp`; the source line in `atx-core/CMakeLists.txt:69-81` and one line in `atx-core/tests/CMakeLists.txt` | - | SQL1 [r SQL-4] | - |
| new `atx-engine/{include/atx/engine,src}/research/store/**` except `catalog/`; `atx-engine/tools/research_store.py`; `atx-engine/tools/record_store.py`, `test_record_store.py` | - | SQL1 [r SQL-4] | SQL4 (cache rows / descriptors, only for a v2) |
| new `atx-engine/{include/atx/engine,src}/research/store/catalog/**`, `atx-research-store`, `atx-engine/schemas/research_store/classes.json`, identity checker, guard test | - | SQL2 | SQL4 (`ingest_ledger.cpp`, the catalog CMake block) |
| `scripts/run_bounded_research.py`, `atx-engine/tools/stage_chain.py`, `scripts/wave_context.py`, `scripts/cycle_resume.py`, `scripts/cycle_verdict.py`, `scripts/wave_stage_record.py`, `scripts/wave_queue.py` (or the `scripts/cycle/` file E2 moved a writer into) | E1 | E2 | SQL3 (wave 3) |
| `atx-impl/src/strategy_ic_signal_cache.cpp`, `strategy_ic_result_cache.cpp`, `strategy_marginal_pair_cache.{cpp,hpp}`, `strategy_marginal_ic.{cpp,hpp}` | S1 | S2 (P16) / none (the marginal verb: S2 forbidden) | SQL4 (wave 3b) |
| `atx-impl/src/strategy_ic_runner.cpp` (the flag and the summary key), `strategy_ic_detail.hpp` (one config field) | D1 | D2 / S2 | D3 (wave 3), then SQL4 (wave 3b, after D3 merges) |
| `atx-impl/CMakeLists.txt` (one link line) | B1 / D1 / S1 appends | C2 | C3 (wave 3), then SQL4 (wave 3b, after C3 merges) |
| `atx-engine/CMakeLists.txt`, `atx-engine/tests/CMakeLists.txt`, `atx-impl/tests/CMakeLists.txt` | appends | SQL1 and SQL2 each append their own block (P2) | SQL4 edits only the SQL2 block and appends one impl test line |

Never touched by an SQL lane: `vcpkg.json`, `CMakePresets.json` [r SQL-4], `wave_manifest.py`, `scripts/cycle/**`
except a file the PM names for SQL3, `backtest_integrity.py`, `research_ledger.py` (B2), the fitter (D-lanes),
`atx/engine/store/**` [j].

---

## 6. (f) Risks, ruling conflicts, rulings applied

### 6.1 Risks and answers

| risk | answer |
|---|---|
| Compile time of the descriptor templates (the ruled cost of SQL-5) | three header tiers; templates instantiated in exactly three TUs (one per group) plus one test TU; consumers include plain row structs and non-template declarations only; nothing in the PCH; folds over `index_sequence`, no recursion; <= 32 columns per table (`consteval` check); runtime DDL strings; root logs the three TUs' compile times (§3.6) |
| Template code written without a compiler (lanes never build) | the descriptor machinery is small (two class templates, one `consteval` factory, six fold-based function templates) and specified in full in brief-SQL1; a first-build failure at root is a slip fixed in place or a fix round (plan §3.4 step 2) [j] |
| Hand-kept per-group schema fixtures drift from the C++ descriptors between a lane commit and root's build | gtests `ResearchStoreSchema.GroupJsonEqualsFixture` / `ResearchCatalog.RecordsSchemaJsonEqualsFixture` fail at root's first build; pytest cannot see C++ (accepted gap; the exact DDL grammar of §3.6 keeps the fixture writable by hand) [j] |
| Concurrent writers on Windows (card \|\| marginal, ref \|\| u, the judge's parallel readers, two IC exes on one cache) | WAL, `BEGIN IMMEDIATE`, 30 s busy timeout, short transactions, bounded retry; one connection per thread or one mutex-guarded connection per process; local NTFS only; tests: two processes (pytest) and two threads (gtest) interleave writes with none lost [w wal.html] [j] |
| Checkpoint starvation and WAL growth | no read transaction spans a computation; `wal_checkpoint(TRUNCATE)` after a catalog run [w wal.html] |
| Kill mid-transaction (the bounded runner kills process trees [c rbr@e1:470-476]) | WAL rollback at next open; never delete `-wal` / `-shm`; `research_gc` deletes whole dirs only [w howtocorrupt.html §1.3] |
| Antivirus / indexer holding `-wal` | transient `SQLITE_BUSY` / `SQLITE_IOERR` retried; no DB under OneDrive; failure in a cache = miss, in a hook = warning, never a changed byte [j] |
| Corruption | catalog: `catalog --rebuild` from the JSON authority; caches: delete the index, read-through re-imports; `quick-check` verb; no backups while every DB is derived (P10 adds `VACUUM INTO` for authority classes) [j] |
| Two SQLite copies | no vcpkg `sqlite3`; the store links `atx::core`, the only owner of `atx_sqlite3` [r SQL-4] |
| Python 3.43.1 vs C 3.53.2 | DDL limited to 3.43 features; pytest applies the fixture DDL on 3.43; a gtest reads a store built by the Python test fixture builder [j] |
| Git-unfriendly binary state | no `.sqlite` committed except one tiny test fixture (marked binary); committed evidence is text: descriptors (C++), the schema fixture, `dump` JSON lines, the catalog digest per wave in the integration log [j] |
| Tamper evidence of the ledger in a mutable DB | JSONL authority through P9; index append-only by trigger; head only from B2's function, compared with the recorded heads; the index never decides N [r SQL-9] |
| Seal and blindness | roots from pins, year-token backstop, declared payload SHAs, `field-source` never followed; lanes use only synthetic fixtures; the real catalog is opened only by root [r SQL-7] |
| Hidden backend state | presence only for the record store and only with the receipt `store` block; explicit flags for the IC and pair caches [r SQL-6] |
| Identity of exes | the new code runs only behind an index file or a flag; each wave's P9-B0 already re-bases exe pins [j] |
| CRLF fixtures | the identity fixture tree keeps its real line ends (`.gitattributes` `-text`; the repo pins `*.jsonl` to LF [c .gitattributes]) [j] |
| NTFS case-insensitivity | `artifact.path_key` is case-folded and `/`-separated (the `research_gc` C-12 precedent [c research_gc.py:16-18]) [j] |
| Spec digests of templates | the catalog does not re-implement `research_spec.spec_digest` (E2's); template digest pins stay `unresolved` in P9 [j] |
| Token cost | four lanes (2 L in wave 2, 1 M in wave 3, 1 L in wave 3b); SQL3 / SQL4 can slip to P10; G-P10 is printed, not gating [r SQL-8] |

### 6.2 Ruling conflicts (SQL-5 against `.agents/cpp/agent.md`)

No part of SQL-5 is impossible under the house rules. Four points need the PM to see how they are met:

1. **§6 "Keep headers clean ... implementation in .cpp files"** vs templates that must be visible where instantiated.
   Met by the tiering of §3.6: the template machinery and the descriptors sit in private headers under `src/` and are
   included only by the three instantiating TUs; public headers hold plain rows and non-template declarations. If the
   PM reads SQL-5 as "every consumer instantiates the templates", that reading conflicts with §6 and with the bounded
   compile time; the design does not adopt it.
2. **§0 / §7 "Tests first" (TDD)** is overridden by the owner's directive (no TDD in this run); the briefs order
   implementation before tests, as every P9 brief does. Not new.
3. **Lane rule 1 "compiles first time"** (brief rule, not house rule) is hardest to meet for template code. The
   design keeps the machinery minimal and fully specified; root's first build is the check. Not a conflict, a stated
   risk (§6.1).
4. **SQL-5 "Python reads the schema the store verb prints"**: the Python accessor reads the identical text from the
   store's own `store_info('schema_json')` row, written by the same function the verb calls, instead of spawning the
   verb. Same bytes, no subprocess, and a store always carries the schema it was created with. If the PM requires the
   subprocess, the accessor calls `atx-research-store schema --json` instead; nothing else changes.

### 6.3 Rulings applied in revision 2

SQL-4: no dependency line; SQL1 owns and fixes `atx-core/src/db/sqlite.{cpp,hpp}` (W1-W8) and owns `record_store.py`.
SQL-5: compile-time C++ descriptors, no generator, generic Python accessor (§3.6). SQL-6: per-cache selection (§3.10).
SQL-7: SQL2 from SQL1's task-1 commit; declared vs verified; root-only real-tree ingest; B2's chain-head function;
pools SQL1 23, SQL2 24. SQL-8: G-P10 printed at the freeze, not gating. SQL-9: JSONL ledger authority through P9,
SQLite index only, P10 owner decision.

### 6.4 Still open for the PM

- **PQ-1** The record store's choice can be written into the run receipt only after SQL3 adds the `store` block (the
  runner is E2's in wave 2). Proposed: in wave 2 the backend exists, prints its selection to stderr (pinned through the
  receipt's `logs`), and only root's identity runs opt in; real cells opt in after SQL3 merges.
- **PQ-2** SQL4 needs the IC flag parse and summary key in `strategy_ic_runner.cpp` / `strategy_ic_detail.hpp` (D3's
  in wave 3) and the link line in `atx-impl/CMakeLists.txt` (C3's): proposed wave 3b after C3 and D3 merge, so no
  cross-lane edit is needed.
- **PQ-3** The Python side of the two flags (spec keys -> argv; `MARGINAL_SPEC_FLAGS` admits only integers
  [c research_cycle@e1:248]) lives in E2's split files; it needs a wave-3 owner (SQL3 if the PM names a file D3 does not
  touch), else root passes the flags by direct argv in identity runs until P10. Also whether the two flags are reuse-
  neutral for resume (`cycle_resume.REUSE_NEUTRAL` [c cycle_resume@e1:49-50]).
