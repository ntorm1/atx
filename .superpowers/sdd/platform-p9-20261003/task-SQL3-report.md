# Lane SQL3 report (P9 wave 3, pool-13): the record-store hook, the receipt `store` block, W3-4, W3-5

## Outcome

Follow-up rounds R10 / R11: item 5 DONE (call lines `b55c0d11`, SQL3-WQ `00779453`, root `175edd61` merged at
`da19d7ac`; teeth 7 / 7; section "Item 5 for the follow-up" below); review 2 BASE `69b8c162`. The paragraph that follows
is the R7 / R8 outcome as written then.

DONE for items 1-4 and SQL3-1 as designed in `handoff-SQL3.md`; item 5 (three call lines in B2's files) is PENDING:
B2's post-identity redo is not merged in root (`git -C C:/atx-wt/pool-2 log --format=%s -30` shows no B2 merge at the
first and the last merge of this round). The round ended at the OWNER STOP after the suite in flight (seed 1)
finished; root's head at the stop is `1b64ce45`, the head already merged, so no further merge was needed. The hook never changes an exit code or a JSON byte; with the catalog off
there is no subprocess and every file keeps today's bytes (tested; evidence below).

## Branch / SHA

`feat/p9-sql3-20261004` in `C:/atx-wt/pool-13`; lease run id `p9-sql3-20261004` (not touched). Commits of this round:
`93760a1e` (merge of root `a15ad887`, clean), `d10d9e54` (code + tests + pre-change evidence), `9e44d3be` (final
merge of root `1b64ce45`, clean: it brings only root's SQL2-CLS `classes.json` row for `atx.nav-spec/v1` and docs,
so the C++ and the build `p9-sql3-a` are those of `d10d9e54`), `784d2abd` (F-2: the seal walk fails closed on a
file-system error; build `p9-sql3-b`), then the report / handoff commit. Every suite below ran on the merged tree
(`9e44d3be` / `784d2abd`; the gtests on both builds). Build tags `p9-sql3-a`, `p9-sql3-b` (equity-dev).
R8 round (close, no build): DIRTY-1 commit `f48d0585` (the handoff as it stood at the stop); `git merge
feat/platform-p9-20261003` = already up to date (root `1b64ce45`); SQL3-SEED: `scripts/tests` under seed 1 re-run on
`f48d0585` (row below); store suite and stage chain already had seed-1 counts (not re-run).

## Required items -> change -> test -> evidence

| # | item | change (file:line at `d10d9e54`) | test | evidence (`sql3-evidence/`) |
|---|---|---|---|---|
| 1 | hook per class; `store` block (SQL-6) | new `scripts/research_store_hook.py` (`selection` :126, `record` :139, `ingest` :162, `check` :195, `catalog_of` :71, `indexes` :112); runner `scripts/run_bounded_research.py:491-494` (selection, `start.json` with `store` last, hook), `:557-562` (`receipt["store"]` after `logs`, hook after the dump); `scripts/wave_queue.py:146` (candidate write), `:350` (emit) | `scripts/tests/test_research_store_hook.py` (8 cases) | `pytest-hook-file-real-exe.log` (8 passed, real exe), `runner-golden-prechange.txt` |
| 1 | stage receipts | `atx-engine/tools/stage_chain.py:118-135` (`Chain(..., on_write=None)`, `written`), `:282`, `:286` (after the failed and the ok write) | `atx-engine/tools/test_stage_chain.py:202` | `pytest-stage-chain-seed{0,1}.log` (10 passed each) |
| 2 | `scripts/research_wave.py` (W3-3) | `:26`, `:41` (`chain_of` passes `on_write` = the hook, class `stage-receipt`) | `test_one_ingest_per_write_in_a_fake_wave`, `test_fake_wave_bytes_equal_with_a_failing_store` | as row 1 |
| 3 | W3-4 (SQL2-S7) `seal_guard.cpp` | `atx-engine/src/research/store/catalog/seal_guard.cpp:40-79` (`kMaxAncestors`, `GitEntry`, `git_entry`, `repo_top`: OUTERMOST `.git` ancestor, bounded walk, an fs error = nullopt), `:162-176` (`sealed_root` scans only below the top; no `.git` ancestor or an fs error = the whole path, as before); header contract `seal_guard.hpp:9-14`, `:58-63`. Callers unchanged. (F-2's fix: commit after `d10d9e54`, build `p9-sql3-b`) | gtest `SealGuard.RootTokenScannedOnlyBelowTheRepoTop` (`research_catalog_seal_test.cpp:84`) | `gtest-p9-sql3-a.log`, `gtest-p9-sql3-b.log` (28 / 28) |
| 3 | N3 (candidates stay keyed by id, W3-4) | documented in the hook's module doc; a re-queued id in a second queue dir is ingest exit 3 = one warning naming `catalog --rebuild` | `test_real_store_exe_ingests_every_class` (last block, real exe) | `pytest-hook-file-real-exe.log` |
| 4 | W3-5 (SQL1-MON) `book_monitor.py` | `atx-impl/tools/book_monitor.py:15-18` (doc), `:242-246` (refusal before `fit_records`: exit 1, nothing written), `:250-257` (`work_indexes`: `W`, `W/..`, `W/*/`, `record_store.INDEX_NAME`). Fitter untouched | `atx-impl/tools/test_book_monitor.py:321` `test_indexed_fit_work_is_refused` (each of the three places) | `pytest-book-monitor.log` (13 passed) |
| 5 | hooks in `cycle_resume.write_binding`, `cycle_verdict.write_verdict` + `keep_copy`, `wave_stage_record.record` | DONE in R10 / R11: `scripts/cycle_resume.py:193`, `scripts/cycle_verdict.py:183`, `:196`, `scripts/wave_stage_record.py:348` (code `b55c0d11`); SQL3-WQ `scripts/wave_queue.py:146`, `:350` (`00779453`); see "Item 5 for the follow-up" | `test_research_store_hook.py` (13 cases), `test_wave_queue.py::test_a_relative_root_hands_the_store_hook_root_relative_names`; teeth 7 / 7 | `r11-teeth-item5.log`, `r11-hook-da19d7ac.log`, `r11-wave-queue-da19d7ac.log`, `r11-identity-da19d7ac.log`, `r11-writers-da19d7ac.log` |

Hook behaviour (all rows): on iff `<root>/build-equity/research-store/catalog.sqlite` and
`<root>/build-equity/bin/atx-research-store.exe` both exist (once per process per root); then
`atx-research-store ingest --catalog DB --class C --path REL --root ROOT --classes <repo classes.json>` (60 s, output
captured, stdin closed), then `research_store_identity.check_one(DB, REL, C, root=ROOT)` (a class without a render
rule, `wave-manifest`: the artifact row holds that class and the file's SHA-256). Any failure: ONE stderr line
`store: WARNING <cls> <relpath>: <why> (the JSON stands; repair: atx-research-store catalog --rebuild)`. Catches
`Exception` only; never raises; writes nothing; adds no path depth (paths recorded root-relative). The runner's hook is
enabled only by its recorded `store` block (`selected=`), never by a second check.

## Tests (no TDD: implemented first, then these)

`scripts/tests/test_research_store_hook.py`:
- `test_selection_finds_the_indexes_a_store_could_select`: `d/index.sqlite`; `d/../index.sqlite` (RecordStore's two
  places; own wins over the parent's); `d/*/index.sqlite`; `--flag=value`; an absolute token; non-directories, odd
  tokens and missing dirs ignored; None when nothing; the catalog alone (no exe) is off; catalog on fills `catalog`;
  decided once per process; `INDEX_NAME == record_store.INDEX_NAME`.
- `test_runner_without_catalog_or_index_writes_todays_bytes`: both files are exactly the runner's render of their doc
  (CRLF on Windows) and the doc equals the PRE-change golden (captured at `93760a1e` before any edit,
  `runner-golden-prechange.txt`; volatile values replaced: `started_utc`, `executable_sha256`, rss / free bytes,
  `wall_seconds`, `owned_processes`, the python path).
- `test_runner_records_an_index_and_the_catalog`: an index under an argv dir -> `store` last in both files, every
  other key where the golden has it; the catalog on with an exe that cannot start -> `catalog` filled
  (`exe_sha256` of the file), exactly two warnings (run-start, run-receipt), exit 0, outcome completed, printed receipt =
  the file.
- `test_hook_off_spawns_and_imports_nothing`: fresh interpreter, `subprocess.run` / `Popen` poisoned: no warning, no
  `sqlite3`, no identity module imported, file bytes unchanged.
- `test_one_ingest_per_write_in_a_fake_wave`: tiny-world fake wave (`test_wave_driver.queued_root`, content digests,
  record date, fixed git dates), fake store: exactly one ingest per stage receipt (9) and per candidate the record
  stage writes (3), each with its class, root-relative `--path`, `--root`, `--catalog`, `--classes`; one check per
  ingest; a resumed (all done) wave calls nothing.
- `test_fake_wave_bytes_equal_with_a_failing_store`: the same root path without a catalog and with a catalog whose exe
  cannot start: every file equal (JSON time-free; git's CRLF notices in commit consoles aside, see "Findings"), exit
  codes equal, 12 warnings (9 stage receipts, 3 candidates); the same for a wave whose screen stage fails (failed
  receipt included; 3 warnings).
- `test_failures_are_one_warning_each`: ingest exit 3 (writer's return value and bytes unchanged), a timeout, a check
  mismatch, a path outside the root: one line each.
- `test_real_store_exe_ingests_every_class` (skipped when the exe is not built): the built exe copied into a temp root,
  `init`; the runner (start + receipt), a chain wired as `chain_of` (ok + failed receipt), two candidates, `emit`, a
  binding, a verdict and its copy, a wave result: no warning; `check_one` None for each; `research_store_identity`
  over the whole catalog: 10 checked, 0 mismatches. A non-canonical `start.json` -> one warning "rendered sha256 ...";
  N3: the same candidate id in a second queue dir -> one warning "ingest exit 3 ...".

`atx-engine/tools/test_stage_chain.py::test_on_write_once_per_written_receipt_never_for_a_done_stage`: once per ok and
per failed receipt, after the write, never for a done stage; a raising callback: one stderr line per call, the same
receipts (time-free), stage calls and exit code as without one.

`atx-impl/tools/test_book_monitor.py::test_indexed_fit_work_is_refused`: no index -> exit 0 (M4 computed); an index
at `W`, at a `W/<role16>-<window>/` store root, at `W/..` -> exit 1, the message names the file, no output dir.

Teeth: the new refusal (W3-5) and the narrowed seal were to be proven by a reverted working-tree mutation. The first
mutation edit was REFUSED by the permission system (see "Permission refusals"); per PERM-1 / r6-wave3-common I did not
retry it or route around it, and did no mutation of the C++ either. Argument instead: the W3-5 test's first run is the
same argv without an index and exits 0 with M4 computed, so without the check every refused case would exit 0 and the
test fails; the W3-4 gtest's `EXPECT_FALSE(sealed_root(<tmp>/env-2025/repo))` fails under the old whole-path rule
(`env-2025` is a token, as `SealedRootNeverOpened` shows for a token anywhere in the path), and its
`EXPECT_TRUE(sealed_root(repo/x-2024))` fails under a nearest-`.git` rule.

## Suites (merged tree; ENV-1 did not occur)

| suite | invocation | result |
|---|---|---|
| gtests (build `p9-sql3-a`) | `atx-engine-research-catalog-tests.exe --gtest_filter=ResearchCatalog*:SealGuard*:StoreCli*` | 28 passed, 0 failed, on each build (`gtest-p9-sql3-a.log` at `d10d9e54`, `gtest-p9-sql3-a-merged.log` at `9e44d3be`, `gtest-p9-sql3-b.log` at `784d2abd`) |
| hook file (real exe) | `pytest scripts/tests/test_research_store_hook.py` | 8 passed (exe of `p9-sql3-a`: `pytest-hook-file-real-exe.log`; of `p9-sql3-b`, seed 0: `pytest-hook-file-real-exe-b.log`) |
| `scripts/tests`, PYTHONHASHSEED 0 | `pytest scripts/tests` | 520 passed, 4 skipped, 0 failed (26 min; real-exe case on `p9-sql3-a`'s exe) |
| `scripts/tests`, PYTHONHASHSEED 1 | `pytest -rs scripts/tests` | 520 passed, 4 skipped, 0 failed (R8 round, head `f48d0585` = code `784d2abd`, real-exe case on `p9-sql3-b`'s exe; 27 min; `pytest-scripts-tests-seed1-r8.log`); skips: `test_cycle_e2e.py:439` (ATX_EQUITY_BIN), `test_research_cycle.py:56`, `:706`, `:946` (RESEARCH_CYCLE_LIVE_ROOT) |
| stage chain, seeds 0 / 1 | `pytest atx-engine/tools/test_stage_chain.py` | 10 / 10 passed |
| store suite, seeds 0 / 1 | `pytest test_research_store_{classes,blind,identity,fixtures}.py test_research_store.py` | 42 passed, 1 failed each: the classes guard (expected, named below) |
| book monitor | `pytest atx-impl/tools/test_book_monitor.py` | 13 passed |

All pytest runs: explicit paths, `-p no:cacheprovider`, own `--basetemp` (scratchpad), `PYTHONDONTWRITEBYTECODE=1`.
Builds (equity-dev, targets `atx-engine-research-catalog,atx-engine-research-catalog-tests,atx-research-store`;
receipts `build-equity/mega-p9-sql3-{a,b}-receipt.json`, kept):
- `p9-sql3-a`: source `d10d9e54`, DirtyEntries 0, exit 0, 80 TUs, 50 s; exes `atx-research-store.exe` `9ae86fa8...`,
  `atx-engine-research-catalog-tests.exe` `e904bf0b...`.
- `p9-sql3-b` (F-2's fix): source `784d2abd`, DirtyEntries 1 (the sprint-dir handoff file only), exit 0, 21 TUs,
  26 s; exes `atx-research-store.exe` `466fcde5...`, `atx-engine-research-catalog-tests.exe` `257f55a8...`.
- Warnings: none in first-party code. The logs hold only clang-cl's driver notice "argument unused during
  compilation: '/MP'" (7 per build, the toolchain's) and, in `-a` (cold tree), two third-party `getenv` deprecation
  warnings in `atx-core/third-party/databento-cpp`.

**Expected red (named):** `atx-engine/tools/test_research_store_classes.py::ClassRegistryGuard::
test_every_schema_literal_is_registered`, listing exactly one literal on the merged tree: `atx.run-store/v1` (this
lane, SQL3-1; in `scripts/research_store_hook.py`, `scripts/run_bounded_research.py`). Before the final merge it also
listed root's `atx.nav-spec/v1`, which root's `ebc918c3` registered. No other red.

## SQL3-1: the new literal and the proposed `classes.json` rows (root adds them; `classes.json` NOT edited here)

- New literal: `atx.run-store/v1` (the receipt `store` block's `schema`; written by `scripts/research_store_hook.py`,
  named in the runner's docstring).
- Proposed rows: add `"registers": ["atx.run-store/v1"]` to the `run-receipt` row and to the `run-start` row (both
  globs end in a literal file name and are disjoint, so `shared_literal_problems` stays empty).
- Checked on an in-memory copy of the merged tree's registry (`sql3-1-proposal-check.py`, `.log`): as committed,
  unregistered = `atx.run-store/v1`; with the proposal, unregistered = [] (the guard turns green);
  `shared_literal_problems` = [].

## New literals, flags, JSON keys, rule ids

- Literal `atx.run-store/v1`. JSON key `store` in `start.json` / `receipt.json` of `atx.bounded-research-run/v1`
  (optional, last, absent by default: the schema id does not change; K-P9-13 addendum). Python API
  `stage_chain.Chain(..., on_write=None)`. No new CLI flag, no spec or manifest key, no rule id. C++: no new symbol in a
  public header (`repo_top` / `holds_git_entry` are file-local).

## Cross-lane edits

None outside SQL3's files. `atx-engine/tools/stage_chain.py` and `test_stage_chain.py`: SQL3's per the brief.
`seal_guard.{hpp,cpp}` + `research_catalog_seal_test.cpp`: SQL3's by ruling W3-4. `book_monitor.py` +
`test_book_monitor.py`: SQL3's by W3-5. `scripts/research_wave.py`: W3-3. No CMake edit (the new gtest is in an existing
TU). Merge conflicts: none (both merges of root were clean).

## For root

**Real-tree catalog run** is root's, at the gate "store" step (M2A-4); this lane opened no real tree and no real
catalog. With SQL3 merged, every bounded run and wave in a tree that holds `build-equity/research-store/catalog.sqlite`
and `build-equity/bin/atx-research-store.exe` ingests its records; a root that does not want that before the gate keeps
the catalog file absent (the hook is off; `start.json` / `receipt.json` keep today's bytes).

**Flag-absent identity, tiny world (root procedure).** Inputs: `sql3-evidence/sql3-fakewave-digests.py` (runs the
tiny-world fake wave, `test_wave_driver.queued_root` with `driver.receipt_digest content` and `record_date 2026-10-01`,
`GIT_AUTHOR_DATE` / `GIT_COMMITTER_DATE` fixed, at one fixed root path, and prints a digest per file: JSON time-free
canonical, others raw bytes).
1. Pre-merge head, then the merge head, same root path:
   `python sql3-fakewave-digests.py <repo> <scratch>/fw > <out>.txt`; expected: the two lists identical (69 files).
   This lane's runs: `fakewave-pre-change.txt` (code at `93760a1e`) = `fakewave-post-change-catalog-off.txt` (code at
   `d10d9e54`) = `fakewave-merged-9e44d3be-catalog-off.txt`: identical, exit 0 each. Substitution list: none (time
   keys are removed by `time_free`); if a commit console differs, drop git's CRLF notice lines first (F-1).
2. Runner: `run_bounded_research.py --root <tmp> --no-git --seconds 60 --max-rss-mib 512 --min-free-mib 64 --output
   <tmp>/x-run -- python -c pass` on both heads: `start.json` / `receipt.json` equal after replacing `started_utc`,
   `executable_sha256`, `sampled_peak_tree_rss_bytes`, `minimum_system_free_bytes`, `wall_seconds`,
   `owned_processes`; no `store` key (pinned by `test_runner_without_catalog_or_index_writes_todays_bytes`).
3. Catalog on (root's choice of tree): initialise `build-equity/research-store/catalog.sqlite` with the built exe in
   `build-equity/bin/`; every file equal to (1) except, by substitution list, the `store` block of each `start.json` /
   `receipt.json` and the digests over those two files' bytes (the stage receipts' / wave result's run timings rows if
   they hash a receipt file; root lists them from the diff); `research_store_identity.py --catalog ... --root ...`
   reports 0 mismatches; no `store: WARNING` line in any stderr.

**What root re-proves at merge.** Build `atx-engine-research-catalog,atx-engine-research-catalog-tests,
atx-research-store`; gtests `ResearchCatalog*:SealGuard*:StoreCli*` (28); `scripts/tests` under seeds 0 and 1; the stage
chain; the store suite under seeds 0 and 1; `atx-impl/tools/test_book_monitor.py`; then SQL3-1's two `registers`
entries (the classes guard turns green); identity (1) and (2) above.

**G-P10 (a)-(e)** (printed, gates nothing, SQL-8):
- (a) pins indexed / `verify --pins` counts: not this lane (SQL2's catalog; root's store step) -> unchanged by SQL3;
  the narrowed seal (W3-4) removes a host-environment false refusal of `catalog` / `ingest` / `cache init --import`.
- (b) render identity of receipt, start receipt, stage receipt, binding, verdict, wave result, candidate: PARTIAL ->
  the writer hook re-renders each record at write time for run-start, run-receipt, stage-receipt, wave-candidate (and
  presence-checks wave-manifest); binding, verdict (+ copy) and wave result wait on item 5 (B2), proven in the
  real-exe test with the exact call; root's real-tree count is the gate's. R11: item 5 landed, so the hook now
  re-renders binding, verdict (+ copy) and wave result at write time too, each writer by its own call (the real-exe
  case: 10 checked, 0 mismatches, with root's store exe `p9-2r`); still PARTIAL for P9 as a whole: SQL3-2 keeps the
  hook OFF on every run of record; root's real-tree count is the gate's.
- (c) two from-scratch catalog digests: root's store step; unchanged by SQL3.
- (d) caches: the receipt `store` block records the record-store index selection (SQL-6), so the choice is now in the
  receipt: LANDED for C1's recording; X-5 cache identities stay root's; SQL-11 holds (no P9 cell uses an index);
  `book_monitor --fit-work` refuses an indexed dir (W3-5).
- (e) every `atx.*/vN` literal registered: PARTIAL until root adds `atx.run-store/v1` (SQL3-1).

## Item 5 for the follow-up (DONE in rounds R10 / R11; Python only, no build)

Rounds: R10 (WIP `489d9a7e`, code `b55c0d11`, merge of root `5c811b56` at `d91ce86c`, handoff `d43b3a8b`; stopped at
OWNER STOP 10 before teeth / report) and R11 (SQL3-WQ `00779453`, teeth, merge of root `175edd61` at `da19d7ac`, this
report). B2's redo has been in root since `fcb0200e`. Review 2: BASE `69b8c162`, file `task-SQL3-review-2.md`.

**Call lines** (each right after its write; `import research_store_hook` at `cycle_resume.py:38`, `cycle_verdict.py:30`,
`wave_stage_record.py:13`; the classes `cycle-binding`, `cycle-verdict`, `wave-result` exist in `classes.json` and name
these writers):

| writer | line | call |
|---|---|---|
| `cycle_resume.write_binding` | `scripts/cycle_resume.py:193` | `record(cycle.res.root, f"{st.run_dir}/{BINDING}", "cycle-binding")` |
| `cycle_verdict.keep_copy` | `scripts/cycle_verdict.py:182-184` | `rel = f"{cycle.cycle_dir()}/{VERDICTS}/{mode}-{k}.json"`; `record(cycle.res.root, rel, "cycle-verdict")`; `return rel` (inside the `with`'s success path) |
| `cycle_verdict.write_verdict` | `scripts/cycle_verdict.py:196` | `record(cycle.res.root, f"{cycle.cycle_dir()}/{VERDICT}", "cycle-verdict")` |
| `wave_stage_record.record` | `scripts/wave_stage_record.py:348` | `record(w.root, w.path(path), "wave-result")` (`Wave.root` is resolved, `wave_context.py:36`) |

Deviation from the R7 plan of this section (slip S5-1, fixed in place in R10): the plan (and the WIP) handed the hook
`cycle.res.path(...)` / `d / ...` / `path`. `Resolver.root` (`scripts/cycle/resolver.py:17`) is `Path(root)` unresolved
and `research_cycle.py --root` is never resolved (`scripts/cycle/cli.py:523`, `:568`), so under a relative `--root r`
those paths are cwd-relative while the hook reads a relative path against the root (`research_store_hook.record`,
`:150`): it would ingest `r/r/...` (one warning, the record not catalogued). The two cycle writers pass the root-relative
name they wrote. Fakes without `.root` (cross-lane, minimal): `atx-engine/tools/test_research_store_identity.py:315`,
`:353`; `atx-engine/tests/fixtures/research_store/make_store_tree.py:100` (`_Res.root = TREE`; no catalog under the
fixture tree, a no-op; the generator was NOT run: it rewrites the tracked fixture tree).

**SQL3-WQ** (ruling SQL3-WQ; item 1's file, `00779453`). `scripts/wave_queue.py:146` carried the same pattern:
`--root` (`:275`) is never resolved (`:303`), so `write` handed the hook `queue_dir(root, d) / ...` = `r/<dir>/<id>.json`
(cwd-relative). Hunk: `research_store_hook.record(root, Path(d or QUEUE_DIR) / p.name, "wave-candidate")` (root-relative;
absolute iff `--dir` is). Deviation: the emit line `:350` had the identical defect (`out = root / a.output` handed over,
`r/w.json`); fixed in the same commit as a second one-line hunk, `research_store_hook.record(root, a.output,
"wave-manifest")` (`a.output` is what `out` is composed from: absolute, or root-relative). With an absolute root both
hunks hand the hook the same file as before (same ingest argv); with the catalog off nothing changes at all. Test:
`scripts/tests/test_wave_queue.py:92` `test_a_relative_root_hands_the_store_hook_root_relative_names`: cwd = tmp,
`--root r`, the hook test's `fake_store` (imported from `test_research_store_hook.py`); `new`, `pin`, `emit`: exactly three
ingests `(q/alpha_a.json, wave-candidate)` x2 and `(w.json, wave-manifest)`, `--root` the resolved root, the bytes at
each ingest = the whole file just written, one check per ingest, no warning. The other hook call sites hand over
resolved paths already (`run_bounded_research.py:452`, `:455`: root and output resolved; `research_wave.chain_of`:
`Wave.root`).

**Tests** (`scripts/tests/test_research_store_hook.py`, R10): `Ingests.files` (:146, :151; the bytes the exe would read at
the call: proves each call is after the write); `fake_store` (:311); `test_one_ingest_per_write_in_a_fake_wave` (:321,
+1 ingest: `out/waves/w1/wave-result.json`, whole at ingest); `test_fake_wave_bytes_equal_with_a_failing_store` (:341,
13 warnings incl. 1 wave-result); `test_write_binding_ingests_its_binding[absolute-root|relative-root]` (:396);
`test_write_verdict_ingests_the_verdict_then_its_copy[no-copy|keep-verdicts]` (:410, relative root);
`test_keep_copy_ingests_each_copy` (:422); `test_real_store_exe_ingests_every_class` (:467) runs the three writers by
their own calls with a real `Resolver` (10 checked, 0 mismatches).

**Teeth** (TEETH-RESTORE in the TEETH-CRLF form; tool `sql3-evidence/r11-teeth-item5.py`, log `r11-teeth-item5.log`, on
`00779453`, seed 0, root's store exe `p9-2r` for the real-exe case): before each mutation `git hash-object <rel>` =
`git rev-parse HEAD:<rel>` and porcelain empty; the tool keeps the bytes (memory + a copy under its basetemp parent);
after the restore the raw SHA-256 of the working bytes = the kept copy's, `hash-object` = HEAD's blob id, porcelain empty
(all seven held; final porcelain empty). The R10 tool (`r10-teeth-item5.py`) had stopped at its precondition before any
mutation (raw bytes vs the LF blob); never rerun.

| id | mutation | file run | red (expected all red; exit 1 each) |
|---|---|---|---|
| M1 | `cycle_resume.py:193` removed | hook file | binding [absolute-root], [relative-root], real-exe (3 failed / 10 passed) |
| M2 | `cycle_verdict.py:183` removed | hook file | keep_copy, verdict [keep-verdicts], real-exe (3 / 10) |
| M3 | `cycle_verdict.py:196` removed | hook file | verdict [no-copy], [keep-verdicts], real-exe (3 / 10) |
| M4 | `wave_stage_record.py:348` removed | hook file | one-ingest fake wave, fake-wave bytes / warnings (2 / 11) |
| M5 | the WIP's cwd-relative form put back at `:193` | hook file | binding [relative-root] only; [absolute-root] and real-exe stay green (1 / 12) |
| M6 | `wave_queue.py:146` back to `p` | `test_wave_queue.py` | the new relative-root case (1 / 5) |
| M7 | `wave_queue.py:350` back to `out` | `test_wave_queue.py` | the new relative-root case (1 / 5) |

Caught 7 / 7.

**Runs** (TEST-1; merged head `da19d7ac`; seed 0; `-p no:cacheprovider`, own `--basetemp`, `PYTHONDONTWRITEBYTECODE=1`,
`C:/Program Files/Python312/python.exe`; env `ATX_EQUITY_ROOT=C:/atx-wt/pool-2/build-equity`,
`ATX_EQUITY_BIN=C:/atx-wt/pool-2/build-equity/bin`, `ATX_RESEARCH_STORE_EXE=<that bin>/atx-research-store.exe`; root's
exes read-only, nothing else read or written under pool-2's `build-equity`):

| file(s) | result | log (`sql3-evidence/`) |
|---|---|---|
| `scripts/tests/test_research_store_hook.py` | 13 passed, 0 failed, 0 skipped (the real-exe case ran) | `r11-hook-da19d7ac.log` |
| `atx-engine/tools/test_research_store_identity.py` (separate invocation) | 16 passed + 6 subtests (its real-exe case ran) | `r11-identity-da19d7ac.log` |
| `scripts/tests/test_wave_queue.py` | 6 passed | `r11-wave-queue-da19d7ac.log` |
| the seven writers' files: `test_cycle_resume.py`, `test_cycle_scoring.py`, `test_research_cycle_roles.py`, `test_research_wave.py`, `test_wave_driver.py`, `test_wave_hardening.py`, `test_wave_eval_verb.py` | 98 passed, 0 failed | `r11-writers-da19d7ac.log` |

EXE-CUR (`r11-exe-sha-before.txt` = `r11-exe-sha-after.txt`, identical: no relink during the run):
`atx-research-store.exe` `d8a500d8...` = Debug `p9-2r` (root M1 / M4 records: current by source); `atx-research-eval.exe`
`47910191...` = Debug `p9-2z` (source `138e1d2c`; receipt copy `m5-logs/mega-p9-2z-receipt.json` in root's sprint dir;
used by `nav_summ` in 4 writer cases); `atx-equity-strategy-ic.exe` `fbb5b392...` and `atx-equity-strategy-targets.exe`
`e6b21410...` = Debug `p9-2y` (root M4 table), hashed as present in the bin `ATX_EQUITY_BIN` names. Earlier rounds:
R10 on `d91ce86c` the same counts (hook 13, identity 16, writers 98; `r10-*-final-d91ce86c.log`); the WIP as committed
3 failed / 5 passed, as its body predicted (`r10-hook-wip-489d9a7e.log`).

Deferred to the gate (TEST-1): whole `scripts/tests` seeds 0 / 1, the stage chain (not edited), the rest of the store
suite, the tiny-world flag-absent identity across commits.

**Merges.** R10 `d91ce86c` (root `5c811b56`), R11 `da19d7ac` (root `175edd61`, root M5 in progress: B2 fix 2, OD3P
follow-up, D3W-PB merged; 0 conflicts; no file of this round changed on root's side, the tests above ran on the merged
tree). No new literal, flag, JSON key or rule id; no CMake edit; no C++.

**What root re-proves at merge (M6).** On the merged head: `test_research_store_hook.py` (real-exe case with a current
store exe), `test_wave_queue.py`, `atx-engine/tools/test_research_store_identity.py`, the seven writers' files (with a bin
holding the eval verb: `ATX_EQUITY_BIN`); with the catalog off (SQL3-2: always in P9) none of the six call lines spawns a
process or changes a byte, so the runs of record are unaffected.

SQL3-TREE: the kept object tree in `C:/atx-wt/pool-13/build-equity` is deleted by name after this report's commit
(handoff head line states the bytes).

## Findings

- F-1 (test hygiene, not a hook effect): a wave's commit console (`consoles/00k-commit.log`) holds git's
  "warning: in the working copy of ..., LF will be replaced by CRLF" once or twice per file depending on whether git
  finds the index entry racily clean (file mtime vs index write time): run timing, not content. Two runs of the same
  wave can differ there; the identity test drops those lines (`GIT_EOL_NOTICE`). Root's identity comparisons of
  consoles need the same substitution.

- F-2 (W3-4, found in self-review, fixed in `p9-sql3-b`): the handoff's design read a file-system error on an
  ancestor's `.git` as "no entry". Had the error hit the real top while a `.git` sat planted in a sealed directory
  below it, the planted one would have become the top and exempted that directory. Fixed: an error other than "not
  found" anywhere on the walk makes `repo_top` answer nullopt, so the whole path is checked (fail closed), as before
  SQL3. Not unit-tested (an fs error on an ancestor cannot be provoked portably); the gtest shows no ancestor of the
  temp dir errs on this host.

## Deviations from the handoff design

- No `Wave.store_record` helper in `wave_context.py` (the handoff's plan for item 5): the direct call is also one line,
  and an unused method now would be dead code until item 5.
- The hook loads the identity module by appending `atx-engine/tools` to `sys.path` (append, not insert: no module of
  `scripts/` can be shadowed) and only when the catalog is on.

## Permission refusals

1. Edit of `atx-impl/tools/book_monitor.py` replacing `require(not indexed, f"--fit-work` with
   `require(True or not indexed, f"--fit-work` (the W3-5 teeth mutation, to be reverted). Refused ("Security Test
   Removal"). Not retried; no other mutation done.
2. `cd C:/atx-wt/pool-13 && rtk proxy grep -n "CODE_PATHSPEC" -A6 scripts/research_tree.py | head -20; rtk proxy grep -n
   "emit" scripts/tests/test_wave_queue.py | head -10` (a read). Refused. Not retried; the tests were written without
   it (the fake-wave roots exclude `build-equity/` through `.git/info/exclude`).

## Ledger candidates

- A wave's commit consoles differ run to run in git's CRLF notices (racy-clean index entries): byte comparisons of
  consoles drop lines starting "warning: in the working copy of".
- `sealed_root` reads the year token only below the OUTERMOST `.git` ancestor (W3-4); a root with no `.git` ancestor is
  checked whole.
