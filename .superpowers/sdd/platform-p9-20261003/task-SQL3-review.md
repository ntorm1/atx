# Lane SQL3 review (P9 RESUME 8; first review; fresh adversarial reviewer)

## Verdict

**APPROVE.** 0 Required, 8 Suggested. None of the Suggested findings blocks the merge.

## Reviewed SHA

- HEAD `a02c074f` (`feat/p9-sql3-20261004`, `C:/atx-wt/pool-13`).
- BASE `1b64ce45`, from REV-BASE: `git merge-base a02c074f feat/platform-p9-20261003`.
  - Root's head is still `1b64ce45`, so the lane is current.
- Both merges of root in the lane's history are plain:
  - `93760a1e` (root `a15ad887`): the tree equals `git merge-tree --write-tree`, `5a8b45b0`.
  - `9e44d3be` (root `1b64ce45`): the tree equals the recomputed tree, `cce158b2`.
- The lane's own hunks are reviewed below.
- EXE-CUR holds. `git diff --name-only 784d2abd..a02c074f` lists sprint-dir files only: no C++ and no CMake.
- The lane's Debug tree `build-equity/` (tag `p9-sql3-b`, source `784d2abd`) matches its build receipt:
  - `bin/atx-research-store.exe`: `466fcde5...`
  - `bin/atx-engine-research-catalog-tests.exe`: `257f55a8...`

## Spec verdict (r7-dispatch "SQL3" 1-6, r8-dispatch "SQL3" 0-1, carry, rulings)

| item | verdict | basis |
|---|---|---|
| r7-1 merge root | DONE | `93760a1e`, `9e44d3be` (plain); the R8 merge was a no-op (root still `1b64ce45`) |
| r7-2 item 1: hook + `store` block (SQL-6) | DONE | See (a) below. |
| r7-2 item 2: `research_wave.py` (W3-3) | DONE | `chain_of` passes `on_write` → hook, class `stage-receipt` (`scripts/research_wave.py:26`, `:41`) |
| r7-2 item 3: W3-4 seal narrowing (SQL2-S7) | DONE | See (b) below. |
| r7-2 item 3: N3 (candidates keyed by id) | DONE | Documented in the hook's module doc; the real-exe case shows a re-queued id is one warning, "ingest exit 3" |
| r7-2 item 4: W3-5 `book_monitor.py` | DONE | `work_indexes` (`W`, `W/..`, `W/*/`); refusal exit 1 before `fit_records`, nothing written; fitter untouched (not in the diff) |
| r7-2 "never changes an exit code or a JSON byte; catalog off = no subprocess, bytes identical" | DONE | My teeth T1-T3 bite; my tiny-world pre-vs-HEAD run is 69 / 69 identical (below) |
| r7-3 SQL3-1 | DONE | See (c) below. |
| r7-4 item 5 (three call lines in B2's files) | PENDING BY RULE | Root `1b64ce45`; `git log --format=%s -30` shows no B2 redo merge. Not a finding. |
| r7-5 build + gtests + pytest groups | DONE | Builds `p9-sql3-a` / `-b`, 0 first-party warnings (lane logs); gtests 28 / 28 (my run); pytest groups in separate invocations |
| r7-6 report with "For root" | DONE | Tiny-world procedure, M2A-4 and G-P10 (a)-(e) present. Accuracy notes: S1, S5. |
| r8-0 DIRTY-1 | DONE | `f48d0585` changes only `handoff-SQL3.md`, as it stood (placeholder included) |
| r8-1 merge + SQL3-SEED | DONE | Merge was a no-op. "SEE-SEED-1" is replaced in `8ad1f9dd` by seed 1: 520 passed / 4 skipped / 0 failed (`pytest-scripts-tests-seed1-r8.log`, lane log). No placeholder is left in the report or the handoff. |
| carry: brief text superseded 1-11 | DONE | Each point checked below. |
| carry SQL3 (required) | DONE | See (d) below. |
| carry SQL3 (optional) | MIXED | E2-4 lock race not done and not listed as not done (S6); N1 / N2 done in effect: the warning names `catalog --rebuild`. |
| SQL2-CLS / All lanes | DONE | The new literal is listed; `classes.json` is untouched |
| B2-12 / W3-12 | HELD | Item 5 waits for B2 |

Detail for the rows above:

- **(a) Item 1: the hook and the `store` block.**
  - `scripts/research_store_hook.py` is new.
  - Runner: `scripts/run_bounded_research.py:491-494` and `:557-562`. `store` is the last key in both files and appears only when `selection` is not None.
  - Stage chain: `atx-engine/tools/stage_chain.py` gains `on_write=None` and `written`.
  - Queue: `scripts/wave_queue.py:146` and `:350`.
- **(b) Item 3: the W3-4 seal narrowing.**
  - `seal_guard.cpp:40-79` and `:162-176`: the outermost `.git` ancestor is the top, and only the part below it is scanned.
  - A file-system error, or no `.git` ancestor, means the whole path is checked (F-2).
  - New gtest: `SealGuard.RootTokenScannedOnlyBelowTheRepoTop`.
  - All callers get the narrowed check unchanged.
  - The "below the top" reading (the top's own name is excluded) matches the SQL2 re-review S7 fix text ("the root-relative remainder").
- **(c) SQL3-1.**
  - `atx.run-store/v1` is listed and `classes.json` is NOT edited.
  - My store-suite run: the guard is the only red, and it names only `atx.run-store/v1`, found in `scripts/research_store_hook.py` and `scripts/run_bounded_research.py`.
- **(d) Required carry items for SQL3.**
  - S7: done, see (b).
  - N3: the id keying is documented.
  - SQL1-MON: done (W3-5).
  - M-4: moved to A4 / P10 by W3-5.
  - Path depth: index paths are recorded root-relative and the hook adds no file.
  - SQL-6 / SQL-2 / SQL-11: held. No spec key, no `REUSE_NEUTRAL` entry, JSON stays the authority.
  - M2A-4: left to root.
- **Brief text superseded, points 1-11.**
  - 1: `research_wave.py` is wired.
  - 2: the hooks go inside `cycle_*`; this is item 5, pending.
  - 3: the writer list is wired except B2's files.
  - 4: `--root` and `--classes` are passed explicitly.
  - 5: `check_one(..., root=)` is called with `root=`.
  - 6: `selection` mirrors `RecordStore`'s `d/` and `d/..`, plus `d/*/`.
  - 7: task 5 is dropped.
  - 8: no cell uses an index.
  - 9: W3-4 is in C++.
  - 10: the `wave-manifest` row lists `atx.research-wave/v2`.
  - 11: the suites run in separate invocations.

**G-P10 (a)-(e), as the report claims and as I read them against plan §0.4:**

- **(a) Pins indexed / `verify --pins`: P10 for SQL3 (not this lane; root's store step).**
  - The report says "unchanged by SQL3", which is accurate.
  - The W3-4 narrowing removes a host-environment false refusal; this was not re-measured.
- **(b) Render identity of the seven record classes: PARTIAL, agreed.**
  - At write time the hook covers run-start, run-receipt, stage-receipt and wave-candidate, and checks wave-manifest for presence.
  - Binding, verdict (and its copy) and wave result wait on item 5.
  - The real-exe case already runs those three writers with the exact call. In my run of it the catalog re-rendered 10 files with 0 mismatches.
- **(c) Two from-scratch catalog digests: root's store step, agreed.**
- **(d) Caches: PARTIAL, not "LANDED" as the report words it (S5).**
  - SQL3 lands only the SQL-6 recording: the receipt `store` block.
  - The X-5 cache identities named in (d) are root's at the gate.
- **(e) Every literal registered: PARTIAL until root adds `atx.run-store/v1`, agreed.**

## Quality verdict

Sound.

- **Flag-absent identity.**
  - Catalog off: the hook returns before any subprocess or `sqlite3` import, and the `store` key is absent.
  - `on_write` defaults to None, and with the catalog off `chain_of`'s lambda is a no-op.
  - No default changed, no key was added unconditionally, and no iteration order changed.
  - `indexes` returns a sorted set.
- **Error paths.**
  - The hook catches `Exception` at every entry and prints exactly one line per write.
  - The ingest timeout is 60 s; the timed-out child is killed by `subprocess.run`.
  - `warn` cannot raise.
  - No outer timeout wraps the bounded runner (checked `scripts/cycle/phases.py`, `wave_context`), so the hook's added up-to-2 x 60 s cannot turn into a kill.
- **Seal.** Nothing is decoded.
  - The C++ walk is bounded and fail-closed on fs error.
  - The Python identity reader checks only the root-relative path, consistent with the narrowed C++ rule.
  - `indexes()` only stats `d/*/index.sqlite` (S8).
- **G-P5.** No Python copy of a C++ rule was added. `indexes` and `work_indexes` mirror `record_store.RecordStore`, which is Python.
- **Scope.**
  - Every touched code file is SQL3's by the brief, W3-3, W3-4 or W3-5.
  - `seal_guard.hpp` changes are comment-only (contract text).
  - No CMake change, no `classes.json` edit, no expected hash or golden edited.
  - The new runner GOLDEN is a fresh pre-change capture at `93760a1e`.
  - No cache paths were committed.
- **C++ against `agent.md` §10.**
  - Loop bounded; `[[nodiscard]]` applied; includes explicit (`<optional>`, `<utility>`); 100 columns held; no lifetime hazard (paths are copied or moved).
  - The lane's logs show 0 first-party warnings under `/W4 /WX`.
  - The F-2 branch is untested (S2), and the bound-exhaustion case is not literally fail-closed (S3).
- **Fingerprints.** No producer, code fingerprint or pin list covers the edited files (grep), so no pinned digest moves.
- **Tests.** They test the contract, not the implementation; T1-T4 bite. One raw-bytes gap (S7).

## Findings

| path:line | severity | problem | fix |
|---|---|---|---|
| `.superpowers/sdd/platform-p9-20261003/task-SQL3-report.md:158`; `handoff-SQL3.md:43` | Suggested (S1) | The report says pre-change = post-change = merged: "identical (69 files)". The committed evidence contradicts this: `fakewave-merged-9e44d3be-catalog-off.txt` differs from `fakewave-pre-change.txt` at `out/waves/w1/consoles/005-commit.log` and `013-commit.log` (raw `diff`; the rtk-wrapped `diff` prints "identical", which is wrong). The cause is F-1, which I reproduced: two HEAD runs at one root differ only at `013-commit.log`, and 0 of 69 differ once git's CRLF-notice lines are dropped. But `sql3-fakewave-digests.py` does not apply that strip, so root's step 1 ("expected: the two lists identical") will show console diffs. | In the tool, drop lines starting `warning: in the working copy of ` from non-JSON files (as `tree_digests` in the hook test does), and restate the result as "identical after F-1". Root can apply the strip at merge (see below). |
| `atx-engine/src/research/store/catalog/seal_guard.cpp:52`, `:66` | Suggested (S2) | F-2's fail-closed branch (`GitEntry::Unknown` → nullopt → whole path) is reached by no test; the lane discloses this. A mutation that treats Unknown as Absent would leave all 28 gtests green. I could not run that mutation: the contract allows no build. | Add a seam, e.g. `detail::repo_top(normal, probe)` with an injectable `.git` probe, and a gtest: the probe answers Unknown at the real top while a `.git` is planted in `x-2024` below → `sealed_root` is true. P10, or a later C++ round. |
| `atx-engine/src/research/store/catalog/seal_guard.cpp:42`, `:64`, `:78` | Suggested (S3) | When the walk exits by `kMaxAncestors`, it returns the `top` found so far (possibly an inner `.git`) instead of nullopt. The comment "far more segments than any path this host can name" is not literally true on Windows: 32,767-character paths allow more than 4,096 one-character segments. This is unreachable in practice. | Return nullopt when the bound is exhausted (fail closed literally), or correct the comment. |
| `atx-engine/include/atx/engine/research/store/catalog/catalog.hpp:27` (SQL2's file, outside W3-4's pair) | Suggested (S4) | The contract comment still says "A root whose absolute path holds a year token 2024-2099 is refused", which is stale after W3-4. | Root or P10: point the comment at `seal_guard.hpp`'s narrowed rule. Not the lane's file. |
| `task-SQL3-report.md:184` | Suggested (S5) | G-P10 (d) is marked "LANDED for C1's recording". Under plan §0.4, (d) is the X-5 cache identities (root's); SQL3 delivers only the SQL-6 recording. "C1" is unexplained. | The freeze print should read (d) PARTIAL: SQL-6 recording landed; X-5 identities root's at the gate. |
| `task-SQL3-report.md` (Outcome / Deviations) | Suggested (S6) | Optional carry items are not accounted for. E2-4 (lock race, W3-2: parked P10, optional for SQL3) is not listed as not done. N1 / N2 are done (the warning names `catalog --rebuild`) but not labelled. | One line each in the item-5 round's report. |
| `scripts/tests/test_research_store_hook.py:209-222` | Suggested (S7) | With the catalog on, the runner's `start.json` / `receipt.json` are compared by content only (`json.loads`); the raw-bytes check `raw == rendered(doc)` exists only in the index-only part. My T3 mutation (the hook appends a byte after its check) left this case green. It was caught by the fake-wave, candidate and real-exe cases instead. | Add `assert raw == rendered(doc)` for `c-run/start.json` and `c-run/receipt.json`. |
| `scripts/research_store_hook.py:122` | Suggested (S8) | `indexes()` globs `d/*/index.sqlite` under every directory the argv names, a data root included. That stats inside child directories whose names hold a year token, which `seal_guard` never descends into, and would record such a path. Nothing is opened or decoded, so this is not a seal breach. | For parity, skip child names that hold a sealed year token (the regex of `research_store_identity.has_sealed_year`). |

## Evidence: my own runs vs the lane's logs

All Python runs used `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider --basetemp <own scratch>` with `PYTHONDONTWRITEBYTECODE=1`, explicit paths, and one invocation per group.

**My own runs:**

- **gtests.** `build-equity/bin/atx-engine-research-catalog-tests.exe --gtest_filter=ResearchCatalog*:SealGuard*:StoreCli*` (exe `257f55a8...`, `p9-sql3-b` = source `784d2abd`; EXE-CUR holds): exit 0, **28 passed** (ResearchCatalog 15, SealGuard 7 incl. `RootTokenScannedOnlyBelowTheRepoTop`, StoreCli 6).
- **Hook file.** `scripts/tests/test_research_store_hook.py`: exit 0, **8 passed**. The real-exe case ran on the `-b` exe.
- **Store suite.** `atx-engine/tools/test_research_store_{classes,blind,identity,fixtures}.py` and `test_research_store.py`: **42 passed, 1 failed, 135 subtests passed**.
  - The failure is `ClassRegistryGuard::test_every_schema_literal_is_registered`: `{'atx.run-store/v1': ['scripts/research_store_hook.py', 'scripts/run_bounded_research.py']}`. This is the expected red under SQL3-1; nothing else is red.
- **Book monitor.** `atx-impl/tools/test_book_monitor.py`: exit 0, **13 passed**.
- **Stage chain.** `atx-engine/tools/test_stage_chain.py` (own invocation): exit 0, **10 passed**.
- **SQL3-1 proposal check.** `sql3-1-proposal-check.py` re-run:
  - As committed, the unregistered list is `['atx.run-store/v1']`.
  - With the proposal, unregistered is `[]` and `shared_literal_problems` is `[]`.
- **Tiny-world identity, catalog off.** My own harness, `fw3.py`, in my scratch; it is the lane's `sql3-fakewave-digests.py` plus two normalisations: repo-path spellings → `<REPO>`, and the F-1 strip.
  - It ran pre-change code (`git archive 93760a1e` of `scripts`, `atx-engine/tools`, `atx-engine/schemas`, `atx-impl/tools`, `atx-impl/strategies`, extracted to scratch) and HEAD, at the same root path.
  - Both exited 0, and **all 69 files are identical**.
  - Two HEAD runs at one root differ raw only at `consoles/013-commit.log`, and are identical after the strip. This is F-1 reproduced.
- **Merges plain.** Both merge trees equal git's recomputed trees (above).

**From the lane's logs only:**

- Build logs `p9-sql3-a` / `-b`: 0 first-party warnings.
- `scripts/tests` under seed 0 (520 / 4 / 0) and seed 1 (520 / 4 / 0, R8 log).
- Store suite and stage chain under seed 1.
- The `-a` exe gtests.

## Teeth

Each mutation changed the CODE under test, never a test. Each was reverted from a byte copy: sha256 equal to the original, `git status` clean after each. There were no permission refusals.

| # | mutation (code under test) | property | result |
|---|---|---|---|
| T1 | `research_store_hook.record`: `catalog = (...) or {"path": CATALOG_REL}` (the hook acts with the catalog off) | catalog off = no subprocess, no warning, bytes | **5 red**: `test_hook_off_spawns_and_imports_nothing` (`done.stderr == ""` fails: the poisoned subprocess was reached), the runner golden (`'store' not in stderr`), the runner index case, the fake-wave equal-bytes case, the candidate case |
| T2 | `record`: `raise SystemExit(3)` after a warning | never changes an exit code | **4 red**: the runner case (`assert 3 == 0`), the fake-wave case, the candidate case, the real-exe case |
| T3 | `record`: append `b"\n"` to the file after the check | never changes a JSON byte | **3 red**: the fake-wave case, the candidate case (`bytes == expect`), the real-exe case (catalog re-render mismatch). The runner's catalog-on case stayed green (S7). |
| T4a | `book_monitor.work_indexes`: drop the `W/*/` place | indexed fit-work refused (fail closed) | **1 red**: `test_indexed_fit_work_is_refused` (`0 != 1` at `.../work/3e3e...-train/index.sqlite`) |
| T4b | `book_monitor.work_indexes`: drop the `W/..` place | same | **1 red**: the same test, at `.../in/index.sqlite` |
| F-2 | `seal_guard.cpp` Unknown branch | the seal walk fails closed on an fs error | **Not runnable:** a C++ mutation needs a build, and the contract allows none. By inspection, no gtest reaches `GitEntry::Unknown` (S2). |

Diagnostics:

- My first T3 mutation script wrote a literal newline into a bytes literal. That made a collection error (`exit 2`) and is void. I reverted it (sha256 equal) and redid T3 with a raw literal.
- The lane's own W3-5 mutation (`require(True or not indexed, ...)`) was refused by the permission system in its round. T4a / T4b are different mutations, made by me and not refused.

## What root must check at merge

1. **SQL3-1 rows in `atx-engine/schemas/research_store/classes.json` (SQL2-CLS).**
   - Add `"registers": ["atx.run-store/v1"]` to the `run-receipt` row (glob `**/receipt.json`) and to the `run-start` row (glob `**/start.json`).
   - Put it as the row's last key, after `"stage": 1`, as the `wave-reader` row does.
   - Against the guard's rule:
     - `registered()` reads `registers`.
     - `shared_literal_problems` allows a literal in two classes only when every glob's last segment is a literal file name and no name is shared. `receipt.json` and `start.json` satisfy that: result `[]`, re-checked by me on an in-memory copy.
     - `test_registry_shape` requires the literal to match `LITERAL`, which it does.
   - Expected: the guard turns from 1 failed to 5 passed.
   - Then the store suite under seeds 0 and 1, and the fixture chain digest `d32f7655...4e69` unchanged, as for `atx.nav-spec/v1` at M2j item 6.
   - The C++ ignores `registers` (no reference in `atx-engine/src` or `include`).
2. **Build before testing (EXE-CUR).**
   - Build `atx-engine-research-catalog,atx-engine-research-catalog-tests,atx-research-store`. The C++ changed: `seal_guard.*` and the gtest.
   - Run gtests `ResearchCatalog*:SealGuard*:StoreCli*`; expect 28.
   - Then `scripts/tests` under seeds 0 and 1. Its real-exe hook case uses `build-equity/bin/atx-research-store.exe`, which must be the merged exe.
   - Then the stage chain, the store suite and `atx-impl/tools/test_book_monitor.py`, each in its own invocation.
3. **Tiny-world identity (report "For root" 1-3), with S1's correction.**
   - Run `sql3-fakewave-digests.py` at one fixed root on the pre-merge head and on the merge head.
   - Before comparing, drop lines starting `warning: in the working copy of ` from non-JSON files (F-1). The tool does not do this. Without the strip, the commit consoles (`00k-commit.log`) can differ run to run, as the lane's own merged list does at 005 / 013.
   - Expected: 69 / 69 identical. No other key may differ.
   - Step 2 (runner, no catalog, no index) is pinned by `test_runner_without_catalog_or_index_writes_todays_bytes`.
   - Step 3 (catalog on): every file equals step 1, except by substitution list: the `store` block of each `start.json` / `receipt.json`, and the digests over those two files' bytes. Root lists the digests from the diff.
   - Also in step 3: `research_store_identity.py --catalog ... --root ...` reports 0 mismatches, and no stderr carries `store: WARNING`.
4. **The hook's on-switch in pool-2 (needs a decision; see PM ruling).**
   - `pool-2/build-equity/bin/atx-research-store.exe` is present today; `build-equity/research-store/catalog.sqlite` is absent. After the merge the hook is therefore off in root until a file appears at that exact path.
   - Once it appears, every later bounded run in pool-2 writes the `store` block into `start.json` / `receipt.json` and ingests: P9-B0, the re-pin runs and the P9 cells alike.
   - Root must therefore order:
     - the gate "store" step (M2A-4), if it writes the catalog at that path;
     - against P9-B0 and the cells, so that references and cells agree on the block;
     - and keep G-P10 (c)'s two from-scratch catalogs at other paths. A hook-fed catalog is not from scratch.
5. **Before the first real run after merge, list any `index.sqlite` under a directory a cell's argv names** (or under its parent, or one level below).
   - `selection` records each one: the `store` block appears and receipt bytes change. SQL-6 intends this, and it is a visible change.
   - SQL-11 says no P9 cell uses an index, so expect none. An index left over from root's identity runs would show up here.
6. **Item 5 stays pending until B2's redo is in root.** Then come the three call lines of the report's "Item 5 for the follow-up" and their tests. A short SQL3 round and a scoped re-review follow.

## Needs a PM ruling

- **Ordering of the gate's store step and P9-B0.** Does the GATE-1 store step create the catalog at the hook's path, `build-equity/research-store/catalog.sqlite`? If so, the hook turns on for every later run in pool-2.
  - Before P9-B0: references and cells both carry the `store` block.
  - After P9-B0: the cells carry it and the references do not. That difference would have to be on the P9-B0 substitution list.
  - Alternatively, the store step writes its catalogs elsewhere and the hook stays off through the cells.
