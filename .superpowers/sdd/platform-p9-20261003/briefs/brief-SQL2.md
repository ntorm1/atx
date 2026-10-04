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

## Lane SQL2: the artifact catalog, `atx-research-store`, and the render-identity checker

**Pool / branch:** pool-24 (ruling SQL-7; `-MaxPool 24`), `feat/p9-sql2-20261003`, run id `p9-sql2-20261003`,
heartbeat `p9-sql2-hb`. **Wave:** 2; base = SQL1's task-1 commit (descriptor machinery, core and cache descriptors,
schema fixtures; ruling SQL-7), rebased on SQL1's merged head before merge (P17 pattern). **Effort:** L. **Serves:**
infrastructure, significance (pins and receipts become queryable evidence). **Merge slot:** wave 2, directly after
SQL1.

**Goal.** Index every artifact the research tree depends on, without moving a byte: the `catalog_records` descriptors
in C++ (ruling SQL-5), a catalog that walks from the pins outward (seal-safe), classifies files, ingests receipts,
stage receipts, bindings, verdicts, wave results, specs, candidates, ledger lines, fields manifests and build receipts
into typed tables and extracts every pin, recording each SHA-256 as `verified` (hashed by the catalog) or `declared`
(stated by a manifest, file not opened; ruling SQL-7); the `atx-research-store` executable, including `schema --json`
that the Python accessor's schema comes from (ruling SQL-5); and a Python checker that proves the rows regenerate the
JSON bytes exactly (stage-2 evidence, ruling SQL-2). Plus the class registry that keeps the inventory a guarded list.

**Read:** `sql-design.md` (all; §1 the class list, §3.6 the descriptor contract, §3.7 the tables, §3.9 the catalog,
§4 the stages); `progress.md` rulings SQL-1..SQL-9; `.agents/cpp/agent.md`; at your base, SQL1's
`atx-engine/src/research/store/detail/{table,table_ops}.hpp`, `tables_core.hpp`, `atx-engine/include/atx/engine/
research/store/{store,digest,rows_core,ops_core}.hpp` and `atx-engine/tests/fixtures/research_store/{schema/*.json,
make_store.py}` (their contracts are in sql-design §3.6 and brief-SQL1 task 1; the `.cpp` bodies arrive with SQL1's
merge); the writers whose bytes you ingest and re-render (read-only): `scripts/run_bounded_research.py:395-501`,
`atx-engine/tools/stage_chain.py:1-90, 255-274`, `scripts/cycle_resume.py:1-60, 180-195`, `scripts/cycle_verdict.py`,
`scripts/wave_context.py:60-80`, `scripts/wave_stage_record.py:220-245`, `scripts/wave_result.py:1-50`,
`scripts/wave_queue.py:1-60, 130-145, 330-345`, `atx-impl/tools/backtest_integrity.py:1017-1167`,
`scripts/research_spec.py:1-60`, `scripts/research-build.ps1:1-40`; A2's
`atx-engine/include/atx/engine/research/fields/{manifest,producer}.hpp` (fields manifest and K-P9-3 shapes);
`atx-engine/include/atx/engine/data/research_window.hpp` (the seal constant). Key structure of real specs only:
`scripts/specs/v8/**` (inputs, open).

**Contracts.** Reads K-P9-13 (SQL1's descriptor API, grammar and digests) and writes its `catalog_records` part
(sql-design §3.7) and the CLI / exit codes (§3.9). Reads K-P9-3 / P6 (producer shapes), K-P9-10 (receipt keys),
K-P9-9 (wave manifest schema), K-P9-1 (field registry).

**Files in scope (owned in wave 2).**
- New public headers `atx-engine/include/atx/engine/research/store/catalog/`: `rows_records.hpp` (plain row structs of
  group `catalog_records`), `ops_records.hpp` (non-template declarations, `records_group()`), `catalog.hpp`,
  `classify.hpp`, `seal_guard.hpp`, `pins.hpp`, `ingest.hpp`, `query.hpp`, `store_cli.hpp`.
- New private sources `atx-engine/src/research/store/catalog/`: `tables_records.hpp` (the descriptors and the
  `pin_status` view SQL), `records_ops.cpp` (the only TU of this lane that includes SQL1's detail headers),
  `catalog.cpp`, `classify.cpp`, `seal_guard.cpp`, `pins.cpp`, `ingest_run.cpp`, `ingest_stage.cpp`,
  `ingest_cycle.cpp`, `ingest_wave.cpp`, `ingest_spec.cpp`, `ingest_ledger.cpp`, `ingest_fields.cpp`,
  `ingest_build.cpp`, `query.cpp`, `store_cli.cpp`, `store_main.cpp`.
- CMake: one block appended to `atx-engine/CMakeLists.txt` (library `atx-engine-research-catalog` linking
  `atx::engine-research-store`, `PRIVATE nlohmann_json::nlohmann_json atx_warnings`, the private include dir
  `${CMAKE_CURRENT_SOURCE_DIR}/src`; executable `atx-research-store`; both `EXCLUDE_FROM_ALL`, no PCH entry), one block
  appended to `atx-engine/tests/CMakeLists.txt` (`atx-engine-research-catalog-tests`).
- Data: `atx-engine/schemas/research_store/classes.json` (the class registry: one row per class of sql-design §1 with
  id, path globs, JSON schema id, writer file, ingest table or `artifact-only`, render rule, pinned-by, stage, plus
  `legacy_allow` rows with an expiry column). Read at run time; not a schema generator.
- Fixtures: `atx-engine/tests/fixtures/research_store/schema/catalog_records.json` (group schema fixture, hand-written
  in the exact printed form, ruling SQL-5); `atx-engine/tests/fixtures/research_store/tree/**` (a synthetic research
  tree: specs with pins, a wave dir with stage receipts, run dirs with start / receipt, a binding, a verdict with
  placeholder numbers, a wave result, two candidates, a 5-line chained ledger, a fields dir with a manifest and two
  declared payloads over 16 MiB named but absent, a build receipt, a `role-2023-2024/` decoy dir holding invalid JSON,
  an unpinned dir) made by `make_store_tree.py` with the real writer calls (byte rules of §3.9, CRLF where the writer
  produces it), and `tree/.gitattributes` (`* -text`: keep line ends as written).
- Python: `atx-engine/tools/research_store_identity.py` (render rules of sql-design §3.9 over SQL1's generic accessor
  `research_store.py`; `--catalog DB --root R [--classes ...]`; `check_one(db, path, cls)` for SQL3),
  `atx-engine/tools/test_research_store_{identity,classes,blind}.py`.

**Forbidden:** SQL1's files (`detail/**`, `tables_core.hpp`, `tables_cache.hpp`, the `rows_*` / `ops_*` headers,
`store.*`, `digest.*`, `research_store.py`, the core / cache schema fixtures): ask the PM for a change; any generated
source file or code-generation step (ruling SQL-5); every `scripts/` file; every atx-impl file; `vcpkg.json`,
`CMakePresets.json`, any dependency line (ruling SQL-4); `backtest_integrity.py` and `research_ledger.py` (B2);
`research_spec.py` (E2). Do not re-implement the ledger chain rule or `research_spec.spec_digest` (G-P5): store line
bytes and line SHA-256 only; `ledger_state.head_sha256` stays NULL (SQL4 fills it through B2); template digest pins are
`unresolved`.

**Cross-lane edits:** none.

**Tasks, in order.**
1. **Descriptors.** `rows_records.hpp` + `tables_records.hpp`: the `catalog_records` tables of sql-design §3.7 with
   keys, `kIndexed`, `kVolatile`, nullability by `std::optional`, `allowed` sets; `trial_line` `append_only`; the
   `pin_status` view (states `ok`, `declared`, `stale`, `missing`, `unresolved`; `declared` = the target's artifact row
   has `sha_source = 'declared'` and the SHA-256 matches). `records_ops.cpp` instantiates SQL1's templates for these
   tables and defines `records_group()` (group version 1, db `Catalog`). `schema/catalog_records.json` by hand in the
   printed form. No typed column may hold a return, IC, Sharpe, turnover, drawdown, DSR / PSR / PBO or NAV value (the
   blind test enforces it). Per-class render metadata (mode `typed` / `doc`, rule) lives in `classes.json`, not in the
   descriptors.
2. **Walker, seal guard, classifier.** Roots from pins (sql-design §3.9): start from `--specs` globs (default
   `scripts/specs/*.json`, `scripts/specs/v8/**`, `scripts/specs/p9/**`), the wave manifests' `out_dir`s, the ledger
   given by `--ledger`, explicit `--root`; follow pins transitively except `field-source`; list everything else under
   `build-equity/` by name only (`skipped_path`). Never open a path holding a standalone year token 2024-2099; never
   open a file over 16 MiB: its row has `sha_source = 'declared'`, `declared_by` = the holder, and the SHA-256 the holder
   states; a file the catalog hashed has `sha_source = 'verified'`; a declared row is never upgraded without hashing
   (`--verify-payloads DIR` hashes the named files and writes `verified`). Sorted walk (path_key order);
   `atx::core::Sha256`; `eol` detected per file.
3. **Ingesters** per class (one TU per family as listed): receipts (typed + `run_command` / `run_binding` children;
   `key_order`, `extra`), start receipts (doc), stage receipts (typed, sorted keys; `.failed-k` files keep their own
   file name), bindings, verdicts and their `verdicts/` copies, wave results (+ `wave_timing`), specs / templates /
   wave manifests / candidates / libraries / recipes / registries / window (`spec_doc`), pins per holder class (spec
   `inputs.*` and `fields.manifest_sha256`, template `locked.*` / `change.inputs.*`, wave manifest `fields.*` /
   `rule_cell.*`, receipt `bindings[]` / `executable_sha256` / `logs`, binding and verdict digests, wave-result
   `manifest` / `receipts{}`, fields manifest entries / `files` / `registry` / `sources`), ledger lines (`trial_line`;
   `ledger_state` without a head), fields manifests (+ entries; producer rows per P6), build receipts (+ `build_exe`).
   JSON parsed with `nlohmann::ordered_json`; a NaN token, an out-of-range number or invalid UTF-8 -> artifact only,
   reason `unparsed`. One `BEGIN IMMEDIATE` per segment of <= 500 files; `catalog_run` row (`files_verified`,
   `files_declared` counted apart) and `catalog_digest` at the end; `wal_checkpoint(TRUNCATE)`.
4. **CLI** `atx-research-store`: `init --catalog DB` (SQL1's `open_store` with `{core_group(), records_group()}`),
   `catalog --catalog DB --root R [--specs ...] [--ledger L] [--rebuild] [--verify-payloads DIR]`, `ingest --catalog DB
   --class C --path P` (one file; idempotent; SQL3 calls it), `verify --pins [--spec S] [--strict]` (counts by state;
   `declared` reported apart from `ok`), `query {artifacts,pins,stale-pins,runs,timings,producers} [--json]` (typed
   columns only, rows by key), `digest`, `dump --table T` (sorted JSON lines), `cache init DIR [--import]` (SQL1's
   `open_cache`; `--import` reads legacy record-store files only, this wave), `cache prune --base DIR` (drops `record`
   rows whose root directory no longer exists), `schema --json --db catalog|cache` (SQL1's `schema_json` over the
   groups of that DB; the bytes Python's accessor sees in `store_info('schema_json')`), `quick-check`. Exit 0 / 2 usage
   / 3 refusal / 4 error.
5. **Render-identity checker** (Python) on SQL1's generic accessor: reads rows, renders with the class's rule from
   `classes.json` and the stored `eol`, reports mismatches by path; `check_one` for a single file. **Class registry
   guard**: every `atx.<name>/v<n>` literal in non-test Python under `scripts/`, `atx-engine/tools/`, `atx-impl/tools/`
   and in C++ under `atx-engine/{include,src}/`, `atx-impl/src/` is in `classes.json` or in its dated `legacy_allow`.
6. Tests (below), report.

**Tests (written after implementing).** gtests (fixture tree): `ResearchCatalog.IngestsSyntheticTree` (row counts per
table), `.DigestReproducibleFromScratch`, `.DigestIndependentOfIngestOrder`, `.PinsResolvedAndStaleDetected` (one pin
edited in a temp copy -> `stale`), `.DeclaredPayloadNeverOpenedAndMarkedDeclared` (`pin_status` = `declared`, never
`ok`), `.VerifyPayloadsUpgradesToVerified`, `.UnparsedJsonCataloguedAsArtifact`, `.CrlfAndLfDetected`,
`.IngestIsIdempotent`, `.TrialLinesAppendOnly`, `.RecordsSchemaJsonEqualsFixture` (`group_schema_json(records_group())`
== `schema/catalog_records.json` bytes); `SealGuard.YearTokenPathNeverOpened` (the decoy dir's invalid JSON would fail
an ingest: the run succeeds and lists it), `.OutsideRootsListedNotOpened`, `.FieldSourcePinsNotFollowed`;
`StoreCli.ExitCodes`, `.QueryRowsOrderedByKey`, `.SchemaJsonEqualsStoreInfo` (the `schema --json` output == the
`store_info('schema_json')` of a store `init` created). pytests: `test_research_store_identity.py` (each render rule
against bytes produced by the real writer calls in a tmp dir, rows inserted through the generic accessor into a store
built by SQL1's `make_store.py` from the three group fixtures; CRLF and LF; a one-byte edit is reported),
`test_research_store_classes.py` (the guard; a planted unregistered schema literal fails),
`test_research_store_blind.py` (no typed column name in `catalog_records.json` matches the statistic list); SQL1's
fixture pytests pass with the third fixture present.

**Root verifies.** Build (equity-dev): `-Targets "atx-engine-research-catalog,atx-engine-research-catalog-tests,
atx-research-store"` plus SQL1's three targets; gtests `atx-engine-research-catalog-tests
--gtest_filter=ResearchCatalog*:SealGuard*:StoreCli*` and SQL1's filters again; pytest `atx-engine/tools` under two
seeds; root logs the compile seconds of `records_ops.cpp` (`scripts\atx-build.ps1 check`). Fixture chain:
`atx-research-store init` + `catalog --root atx-engine/tests/fixtures/research_store/tree` then
`research_store_identity.py` -> 0 mismatches; root records the fixture catalog digest once (as T1's canary goldens)
and every later build must reproduce it.
**Real tree (root-only bounded run, ruling SQL-7):** 0 trials, through `run_bounded_research.py` (<= 600 s, <= 4,096
MiB): `init --catalog build-equity/research-store/catalog.sqlite`, `catalog --root .`, `verify --pins` (counts by kind
and state, `verified` and `declared` apart; every stale or missing pin listed by path), `research_store_identity.py`
over classes run / run_start / stage_receipt / cycle_binding / cycle_verdict / wave_result / candidate / trial_line ->
0 mismatches of N (logged), a second catalog from scratch -> the same catalog digest; wall and peak RSS logged. The
catalog file holds verdict and wave-result documents and is opened only by root; lanes and reviewers use the fixture
tree.
**SQL1's opt-in identity (needs this lane's `cache init`):** 0 trials, quiet host: re-run X-5's fit and card commands
(the argv of X-5's fit / card receipts, outputs renamed, through `run_bounded_research.py`, as R0-3's `v8-i16d-x5-*`
runs did) three times under one build: A with no index; B after `atx-research-store cache init build-equity/fit-work`
(cold); C again (warm). Expected: every output file of A, B and C byte-identical (fit: `composition_weights.json`,
`admission.json`, `admission.csv`; card outputs); B's and C's records are rows of `build-equity/fit-work/index.sqlite`;
B's and C's stderr logs hold the `record_store: index sqlite` line. No substitution list. Then delete the index (no real
research cell uses it before SQL3's receipt block merges; ruling SQL-6, PQ-1).

**Flag-absent identity (root procedure).** Nothing existing changes except two CMake appends; no research executable
links the new libraries: `git diff --stat <base>..<sha> -- atx-impl scripts` empty; research outputs unchanged by
construction.

**Out of scope:** writer hooks and the receipt `store` block (SQL3), IC caches and the ledger head (SQL4), C++ writers
(NAV, IC, fitter, fields) dual-writing (P10), any pin check that refuses a run (the cycle's checks stay Python,
E-lanes).
