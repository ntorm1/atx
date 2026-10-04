# Lane SQL2 review

## Verdict
**APPROVE**. There are 0 Required findings and 6 Suggested findings (all minor).
- Spec compliance: **PASS**.
- Task quality: **PASS, with suggestions**.

## Reviewed SHA
- Lane head: `e835b434` (report commit). Code head: `6954b605`. Branch: `feat/p9-sql2-20261003`. Pool: `C:/atx-wt/pool-23`.
- How the diff was built: the lane's own commits only. Base = `51ce19f2` with SQL1's fix head `1a5b051d` merged in
  (`git merge-tree --write-tree 51ce19f2 1a5b051d` -> tree `9384c5b8`). The diff runs from that tree to `e835b434`:
  76 files, +10,731 / -0.
- Merge integrity:
  - `afea225d` adds exactly SQL1's `4d198fef..1a5b051d` patch. The two diffs are byte-equal once `index` lines are
    dropped.
  - `51ce19f2` resolved its one conflict (`atx-engine/tests/CMakeLists.txt`) by keeping both blocks.
  - SQL1's files are untouched by the lane's own commits. The only modified (non-new) files are the two CMake tails.

## Evidence (run by the reviewer, in pool-23)
1. gtests. Command:
   `build-equity\bin\atx-engine-research-catalog-tests.exe --gtest_filter='ResearchCatalog*:SealGuard*:StoreCli*'`
   ```
   [==========] 21 tests from 3 test suites ran. (15583 ms total)
   [  PASSED  ] 21 tests.
   exit=0
   ```
   - The binary is build `p9-sql2-c` (receipt `build-equity/mega-p9-sql2-c-receipt.json`, ExitCode 0).
   - Since `56469cec`, no `.cpp`, `.hpp`, CMake or fixture file has changed. The only change is the description string
     in `classes.json`, which is read at run time and not used by the C++.
   - Build logs a / b / c contain no warnings under `/W4 /WX`. b's only errors are the `Run` name clash that c fixed.
2. pytest. `PYTHONDONTWRITEBYTECODE=1`, `-p no:cacheprovider`, explicit paths:
   `test_research_store_{classes,blind,identity,fixtures}.py` and `test_research_store.py`.
   ```
   == PYTHONHASHSEED=0
   42 passed, 119 subtests passed in 10.26s
   exit=0
   == PYTHONHASHSEED=1
   42 passed, 119 subtests passed in 6.34s
   exit=0
   ```
   There were no skips, so `FixtureChain` ran.
3. Fixture chain, from scratch, run twice: once with a relative `--root` and once with an absolute one.
   ```
   files_seen 29 verified 25 declared 0 skipped 4
   catalog_digest d32f7655e6cdd860e1af36ceadb00a1c1603136a69d17ad954e629cae4e84e69   (both runs)
   research_store_identity.py ... total checked 12 mismatches 0   exit=0
   ```
   This reproduces the report's canary digest.
4. Flag-absent identity. `git diff --stat 1239a5ff..HEAD -- atx-impl scripts` printed nothing. Both new targets are
   `EXCLUDE_FROM_ALL`, and no existing target or tool changed.
5. Probe of the relaxed `test_registry_shape` (scratch script, since deleted). Each case inserts one extra class that
   shares `atx.bounded-research-run/v1`, placed ahead of `run-receipt`. Result:
   - baseline: passes;
   - `build-equity/**`: passes;
   - `**/*receipt.json`: passes;
   - `**/*`: passes;
   - `**/*.json`: fails;
   - the same glob as an existing class: fails.
6. Duplicate-key probe (scratch copy of the fixture tree, since deleted). I added
   `scripts/specs/p9/candidates/fx_alpha_a.json`, which has the same `id` as the v8 file but a different `status`.
   - The catalog keeps one `candidate` row, for the v8 path.
   - The identity checker reports
     `scripts/specs/p9/candidates/fx_alpha_a.json [wave-candidate]: 0 candidate rows for the file`, exit 1.
7. Seal regex on hex names. Out of 200,000 random 64-hex file names, 1.07% match `(^|\D)20(2[4-9]|[3-9]\d)(\D|$)`.

## Spec compliance (brief-SQL2 + wave2-carry SQL2 + rulings)
| item | status |
|---|---|
| Task 1: descriptors, `kIndexed` / `kVolatile`, optional = nullable, `allowed` sets, `trial_line` append-only, `pin_status` with 5 states (`declared` apart), `records_group()` v1 db Catalog, hand-written fixture | met (`tables_records.hpp`, `records_ops.cpp`; `RecordsSchemaJsonEqualsFixture` green) |
| Task 2: roots from pins, sorted walk, seal backstop, 16 MiB never opened, `declared` vs `verified`, `--verify-payloads` is the only upgrade, build-equity listed by name, `eol` per file | met (`catalog.cpp`, `seal_guard.cpp`; see S2 on the root path itself) |
| Task 3: ingesters per family, `ordered_json`, NaN / out-of-range / UTF-8 -> unparsed, segments of <= 500 in `BEGIN IMMEDIATE`, `catalog_run` with verified / declared apart, digest, `wal_checkpoint(TRUNCATE)` | met |
| Task 4: CLI verbs and exit codes 0 / 2 / 3 / 4 | met (`StoreCli.ExitCodes`); SQL1-MON warning in `--help` and on `cache init` stderr |
| Task 5: render-identity checker, `check_one`, class registry guard over the named dirs | met |
| Task 6: every named gtest and pytest | present and green (plus 4 extra gtests) |
| SQL-7: declared / verified, never sealed or large, field-source pins not followed | met (`SealGuard.*`, `DeclaredPayloadNeverOpenedAndMarkedDeclared`) |
| SQL-9 / G-P5: no chain rule, no `spec_digest`, `head_sha256` NULL, single seam | met (`ingest_ledger.cpp:33`, `LedgerHeadSeamReceivesLinesInFileOrder`) |
| SQL-11 / SQL-6: `cache init` is the only creator; `open_cache` never creates | met (`create_cache` only; `StoreCli` checks that readers never create) |
| SQL-4 / SQL-5: no dependency line, no code generation; schema JSON comes from the descriptors | met |
| SQL2-CLS: the lane's new literal is listed | met (`atx.research-store-classes/v1` under `domains`) |
| W2-BUILD: lane builds through `research-build.ps1` | met (tags p9-sql2-a/b/c) |
| Forbidden files (SQL1's, `scripts/**`, atx-impl, `vcpkg.json`, presets, B2 / E2 files) | untouched |
| Blind: no typed column holds a statistic | met (`test_research_store_blind.py`; query verbs print typed columns only) |

Deviations accepted:
- three private helpers in the owned directory;
- `cycle_binding.spec_sha256` nullable;
- extra registry keys `seed_globs`, `registers` and `domains`;
- the `git_tracked` heuristic;
- pool-23 instead of pool-24 (POOL-MAP ruling).

## The resume fix to `test_registry_shape` (6954b605): verdict
**The relaxation is sound in intent and correct for today's registry. It does not weaken the registration guard.
Its check is weaker than its own comment claims.**
- **Root cause confirmed.** `run_bounded_research.py:423,434` builds one dict with
  `schema="atx.bounded-research-run/v1"` and writes it to both `start.json` and `receipt.json`. The old
  "one class per literal" rule was wrong for this real writer.
- **The two classes really are disjoint.** `**/receipt.json` and `**/start.json` have different literal leaf names.
  `ResearchCatalog.ClassifierUsesGlobsAndSchemas` shows that both classes are reachable.
- **No expected value was edited.**
- **The registration guard is intact.** `test_every_schema_literal_is_registered` takes the union of `schemas`, so
  duplicates never mattered to it.
- **The new check is approximate.** It compares glob strings and rejects only the two spellings `**` and `**/*.json`.
  Probe 5 shows it accepts overlapping or catch-all globs. The comment "the first-match rule can never make the later
  class dead" is therefore not enforced. See S1.

## Findings
| # | path:line | tag (severity) | problem | fix |
|---|---|---|---|---|
| S1 | `atx-engine/tools/test_research_store_classes.py:122-135` | Suggested (minor) | The shared-literal rule checks only string inequality and two catch-all spellings. `build-equity/**`, `**/*receipt.json` and `**/*` pass even when they shadow `run-receipt` / `run-start` (probe 5). Root edits `classes.json` at every merge slot (SQL2-CLS), so a silent overlap would misroute files. | Enforce the invariant the comment states. Either require every glob of a shared-literal class to have a wildcard-free last segment, with those leaf names pairwise disjoint across the sharing classes, or pin an explicit allow-list `{"atx.bounded-research-run/v1": ["run-receipt", "run-start"]}`. |
| S2 | `atx-engine/src/research/store/catalog/store_cli.cpp:283,309`; `seal_guard.hpp:6-11` | Suggested (minor) | The seal backstop tests only root-relative paths. A `--root` (for `catalog` or `ingest`) that itself names a sealed directory, such as `.../v8-2024-oos`, opens everything under it. The real run uses `--root .`, so nothing is exposed today. This is defence in depth. | Refuse with exit 3 when `has_sealed_year(utf8_path(normal_root(root)))` is true, in both `cmd_catalog` and `cmd_ingest` (or in `run_catalog` / `ingest_one`). |
| S3 | `atx-engine/src/research/store/catalog/cache_index.cpp:70-87,95-102` | Suggested (minor) | `cache init --import` reads partition, kind and record files with no seal check. This is the one read path that bypasses `has_sealed_year`. Root's X-5 run uses `cache init` without `--import`, so it is not exposed. | Skip, and count as rejected, any partition or kind directory whose name holds a sealed year token. File names are SHA-256 hex, see note R5. |
| S4 | `atx-engine/src/research/store/catalog/ingest_spec.cpp:159-167`; `catalog.hpp:22-24` | Suggested (minor) | `candidate` is keyed by `id` (and `build_receipt` by `Tag`), so two walked files with one key resolve last-writer-wins. Which file wins depends on walk order, which contradicts "in any walk order, reproduces it" (`catalog.hpp:23-24`). The losing file's row disappears silently. The identity checker does catch it (probe 6). `DigestIndependentOfIngestOrder` covers only a tree without duplicates. | Make the collision deterministic and visible. Keep the smallest path, record the other as `skipped_path` reason `unparsed` (or refuse with exit 3), and narrow the header claim to "keys unique in the tree". |
| S5 | `atx-engine/src/research/store/catalog/catalog.cpp:354,575-577,601` | Suggested (minor) | `files_seen = verified + declared + skipped` counts an unparsed file twice (it is both verified and skipped `unparsed`) and counts a seal-named directory as one file. The fixture totals are 29 and 31, against 28 and 30 distinct entries. | Count each unparsed file once (subtract the `unparsed` skips), or document that `files_seen` counts rows. |
| S6 | `atx-engine/src/research/store/catalog/store_cli.cpp:265-268,130-134` | Suggested (minor) | `catalog --rebuild` on a non-catalog file returns 4. `open_failure` maps the same InvalidArgument to refusal (3) everywhere else. | Use `open_failure` (or map InvalidArgument to 3) for the error from `remove_catalog`'s open. |

Hygiene, not a code finding: pool-23 holds `atx-db/src/atx_db/__pycache__/` (17:45, during the lane's resumed
full-suite run) and `.mypy_cache` dirs under `atx-engine/`. All are ignored and regenerable. Under ruling DISK the lane
deletes them.

## What root must check at merge
- **R1. Classes guard after slots 1-10.** At slot 11, `test_every_schema_literal_is_registered` turns red for any
  literal that slots 1-10 add. Root makes one SQL2-CLS commit per slot. After each such commit, re-run the fixture
  chain. The canary digest depends on how fixture files classify (class ids and order), not on the bytes of
  `classes.json`.
- **R2. Build and tests.**
  - Rebuild on the SQL1 merged head, including root's SQL1-N1 fix.
  - Log the compile seconds of `records_ops.cpp`.
  - Run the equity-hygiene include check on the catalog TUs. Several TUs rely on transitive includes through
    `catalog_detail.hpp`.
  - Run pytest only after building `atx-research-store`. Otherwise `FixtureChain` skips silently, and it is the one
    test that catches drift between the Python `TYPED` / `_where` mapping and the C++ ingest.
- **R3. Canary golden.** Record `d32f7655e6cdd860e1af36ceadb00a1c1603136a69d17ad954e629cae4e84e69`
  (29 seen / 25 verified / 0 declared / 4 skipped).
- **R4. Real-tree bounded run.**
  - Use `--root .` only (S2).
  - Log the `skipped_path` count by reason. The build-equity CMake tree is listed by name.
  - Check for duplicate candidate ids and build tags before trusting digest equality across walk orders (S4). An
    identity mismatch reading "0 candidate rows" means a duplicate.
- **R5. Expect seal-regex false positives on hex names.** About 1.07% of SHA-256-named files, such as record-store or
  cache entries, carry a "standalone" 20xx token. They are listed `seal-name` and never opened, so pins to them read
  `missing`. This is conservative, not a seal breach. Do not read those rows as sealed data.
- **R6. B2 seam.** SQL4 / root binds B2's chain-head function in `default_ledger_head()` only
  (`ingest_ledger.cpp:33`). `LedgerHeadSeamReceivesLinesInFileOrder` pins the contract.

## Checked
- [x] `.agents/cpp/agent.md` §10 applied to the diff. Findings:
  - no UB or narrowing; every loop is bounded and commented;
  - lifetimes: the `DbWrite` lambdas capture by value; `FileView` / `PinScope` references are used synchronously;
    `CaseResolver` keeps references into a `std::map`, which stay stable;
  - error paths return `Result` / `Status`;
  - `/W4 /WX` clean per build logs a / b / c; every C++ line is at most 100 columns.
- [x] The diff stays inside the brief's files in scope, apart from the listed private helpers and the CMake tail
  appends. No SQL1, `scripts/`, atx-impl or `atx-db/` file was touched by the lane's commits.
- [x] The report's evidence matches its claims: 21/21 gtests, 42 pytests under two seeds, the canary digest, the empty
  flag-absent diff and the root cause of the shape-test failure were all re-run or re-checked. The 450-test full suite
  was not re-run.
- [x] Seal and blindness:
  - every walk path checks `has_sealed_year` before listing or opening;
  - the CaseResolver ignores symlinks, 8.3 short names and `..`;
  - `content_digests` re-checks seal and size;
  - the checker's `_read_file` is guarded;
  - typed columns and the query verbs hold no statistic;
  - the remaining gaps are S2 and S3.
- [x] Digest determinism:
  - volatile tables and columns are excluded;
  - `declared_by` is the smallest holder;
  - the pending map is sorted;
  - the digest is independent of root spelling (evidence 3);
  - the remaining gap is S4.
- [x] Flag-absent byte identity: `git diff 1239a5ff..HEAD -- atx-impl scripts` is empty, both targets are
  `EXCLUDE_FROM_ALL`, and nothing existing is linked or edited.
- [x] Reviewer processes and scratch: no process left running. The scratch DBs, tree copies, probe script and pytest
  basetemps were deleted. The gtest temp root is absent.
