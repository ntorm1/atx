# Lane SQL3 review 2 (P9 RESUME 11; the item-5 + SQL3-WQ follow-up; fresh reviewer, REV-FRESH)

## Verdict

**APPROVE.** 0 Required, 4 Suggested. None of the Suggested findings blocks the merge.

## Reviewed SHA

- HEAD `0ebb83da` (`feat/p9-sql3-20261004`, `C:/atx-wt/pool-13`, tree clean at start and end).
- BASE `69b8c162` (FU-REVIEWS; review 1's APPROVE commit). Range `69b8c162..0ebb83da` only.
- Commits of the lane in the range:
  - code: `489d9a7e` (WIP item 5), `b55c0d11` (item 5 fixed: root-relative names, fakes, tests), `00779453` (SQL3-WQ);
  - records: `d43b3a8b`, `b995f926`, `0ebb83da` (sprint dir only).
- Root merges in the range: all three are plain. Each merge tree equals `git merge-tree --write-tree <p1> <p2>`:
  - `2d02466e` (root `73b4acfd`): `a49c8a2f` = recomputed.
  - `d91ce86c` (root `5c811b56`): `4d87050f` = recomputed.
  - `da19d7ac` (root `175edd61`): `fbca1a77` = recomputed.
- The lane's net change against the root it last merged (`git diff 175edd61 0ebb83da`), outside the sprint dir, is
  nine Python files:
  - `scripts/cycle_resume.py`, `scripts/cycle_verdict.py`, `scripts/wave_stage_record.py` (item 5);
  - `scripts/wave_queue.py` (SQL3-WQ);
  - `scripts/tests/test_research_store_hook.py`, `scripts/tests/test_wave_queue.py` (tests);
  - `atx-engine/tools/test_research_store_identity.py`, `atx-engine/tests/fixtures/research_store/make_store_tree.py`
    (fakes gain `root`: the cross-lane minimum, listed by the lane).
- There is no C++, no CMake, no `classes.json` change, no golden, and no expected hash or pin.
- Trial merge with root's current head `bba4c06f`: `git merge-tree --write-tree 0ebb83da bba4c06f` exits 0 (tree
  `d6777122`, no conflict). The three test files pass on that tree (see Evidence).

## Spec verdict (r11-dispatch "SQL3-5-FINISH" 1-5, r10-dispatch "SQL3-5" 1-4, rulings)

| item | verdict | basis |
|---|---|---|
| r10-1: call lines vs the handoff's step 2; one `import research_store_hook` per file | DONE | Imports at `cycle_resume.py:38`, `cycle_verdict.py:30`, `wave_stage_record.py:13`. Calls at `cycle_resume.py:193`, `cycle_verdict.py:183` (keep_copy), `:196` (write_verdict), `wave_stage_record.py:348`. Slip S5-1 (the WIP's cwd-relative `cycle.res.path(...)`) is fixed in place and recorded. |
| r10-2: tests (fake wave +1 ingest; one fake-store case per writer) | DONE | `test_one_ingest_per_write_in_a_fake_wave` (:321) and `test_fake_wave_bytes_equal_with_a_failing_store` (:341) count the wave result. `test_write_binding_ingests_its_binding` (:396, absolute and relative root), `test_write_verdict_ingests_the_verdict_then_its_copy` (:410), `test_keep_copy_ingests_each_copy` (:422). The real-exe case (:467) now runs the two cycle writers by their own calls. |
| r11-1 SQL3-WQ (+ SQL3-WQ-2) | DONE | `wave_queue.py:146` passes `Path(d or QUEUE_DIR) / p.name`; `:350` passes `a.output`. The one test `test_wave_queue.py:92` covers both lines: `new` and `pin` reach `:146`, `emit` reaches `:350`. Both are listed in the report. |
| r11-2 teeth (TEETH-CRLF form, one per call line) | DONE | See "The teeth evidence" below. |
| r11-3 runs (TEST-1, seed 0, root's exes read-only, SHA-256 logged) | DONE | Lane logs `r11-*-da19d7ac.log`; exe SHA files before = after. I re-ran the three files named for review 2 (below); the seven writers' files are from the lane's log only. |
| r11-4 root merge, report section, SQL3-WQ paragraph, handoff head line, commit | DONE | `da19d7ac` is plain. `task-SQL3-report.md` "Item 5 for the follow-up" holds the call lines, S5-1, SQL3-WQ, teeth, runs, EXE-CUR and root's re-proof list. The handoff head line is updated. |
| r11-5 SQL3-TREE | DONE | `C:/atx-wt/pool-13/build-equity` now holds only `audits/`, `v8-interim3-pitch-render-run2/`, the `mega-p9-sql3-{a,b}-*` receipts / logs and `vcpkg-manifest-install.log` (2,298,485 bytes, my `du`). `git ls-files -d build-equity` is empty. |
| SQL3-2 | HELD | `C:/atx-wt/pool-2/build-equity/research-store` is absent before and after every run of mine (existence check only). |
| SQL2-CLS | HELD | No `atx.*/vN` literal is added or removed in the lane's code diff (scan of every +/- line). |
| SQL3-1 | n/a this round | Registered by root at `fd084e78`. |

### The teeth evidence

- Tool: `sql3-evidence/r11-teeth-item5.py`, log `r11-teeth-item5.log`, run on `00779453`.
- Precondition before every mutation: `git hash-object <rel>` = `git rev-parse HEAD:<rel>`, and porcelain is empty.
- The tool keeps the bytes itself, in memory and as a copy, and restores them in `finally`.
- After every restore:
  - the raw SHA-256 of the working bytes = the kept copy;
  - `hash-object` = HEAD's blob;
  - porcelain is empty.
- The tool never compares raw bytes with `cat-file blob`.
- The seven mutations are the ones the report's table claims. M1-M4 remove one item-5 call each, M5 puts the WIP
  form back, and M6 / M7 put the SQL3-WQ old forms back.
- Each mutation's red set in the log equals the report's (M1-M3 3 / 10, M4 2 / 11, M5 1 / 12 with its two
  must-stay-green cases green, M6 / M7 1 / 5).
- Caught 7 / 7; the final porcelain is empty.

## Quality verdict

Sound.

- **Exit code and JSON bytes.**
  - Each call is the line right after its file is closed. `write_text` closes the file; `keep_copy`'s call follows its
    `with`; `Wave.write_json` closes in its `with`.
  - `record` catches every `Exception` and returns None. The writers' return values and control flow are unchanged:
    `keep_copy` still returns the same root-relative name.
  - The only expressions evaluated outside the hook's `try` are `cycle.res.root`, the f-strings, `w.root`,
    `w.path(path)`, `Path(d or QUEUE_DIR) / p.name` and `a.output`. Every production caller has these:
    - `Cycle.res` is always a `Resolver`, which carries `.root` (`scripts/cycle/phases.py:123`).
    - The only production callers are `cycle/cli.py:389` and `cycle/gate.py:116`.
  - The three test fakes without `.root` were the only other callers, and the lane gave each of them `root`.
- **One ingest per write.**
  - Each of the four writes has exactly one call. `write_verdict`'s own call ingests the verdict; the copy is ingested
    by `keep_copy`'s call, so the verdict is never ingested twice.
  - The tests assert the exact `(path, class)` list in call order, and the bytes the exe would read at each call
    (`Ingests.files`).
  - My R1 (the wave-result call doubled) and R2 (the verdict call moved before its write) both go red (below).
- **Catalog off = no subprocess.**
  - The new calls go through the same `record` → `catalog_of(base) is None` → `return` as item 1.
  - `test_hook_off_spawns_and_imports_nothing` poisons `subprocess` in a fresh interpreter and still passes.
  - The writers' own files (catalog-free test roots) pass, per the lane's log.
- **Root-relative names at every call site** (grep of every `research_store_hook.record` call at HEAD):

  | call site | root handed | path handed |
  |---|---|---|
  | `cycle_resume.py:193`, `cycle_verdict.py:183`, `:196` | `cycle.res.root`, unresolved | the root-relative name the writer used |
  | `wave_queue.py:146` | `--root`, unresolved | `Path(d or QUEUE_DIR) / p.name` (absolute only if `--dir` is) |
  | `wave_queue.py:350` | `--root`, unresolved | `a.output` (absolute only if `--output` is) |
  | `wave_stage_record.py:348`, `research_wave.py:41` | `w.root`, which `Wave.__init__` resolves | absolute under the resolved root |
  | `run_bounded_research.py:494`, `:562` | root resolved at `:452` | output resolved at `:455` |

  - The two absolute forms are not the cwd-relative defect.
  - No `cycle.res.path(` or other cwd-relative path reaches the hook.
  - With an absolute root, every rewritten line hands the hook the same file as before, so the ingest argv is the
    same. This includes `--dir` / `--output` that are absolute or hold `..`, since the hook resolves both forms
    identically.
- **SQL3-WQ test.**
  - Under a relative `--root r` it asserts three ingests, `(q/alpha_a.json, wave-candidate)` twice and
    `(w.json, wave-manifest)`, with `--root` = the resolved root, the whole file at each ingest, and no warning.
  - The lane's M6 and M7 each turn it red.
- **House style.**
  - Python only. No added line exceeds 120 columns.
  - No rule logic was added, so G-P5 does not apply: nothing mirrors a C++ rule.
  - The comments name the lane item.
  - No fingerprint or pin lists the edited files: the only JSON hits are `classes.json`'s `writer` text and D2R's
    scan evidence.
- **Scope.**
  - Item 5 sits in B2's three files, as ruled.
  - SQL3-WQ is in item 1's file.
  - The two `atx-engine` edits are one attribute each, on fakes. `make_store_tree.py` was not run, so the tracked
    fixture tree is unchanged, and no hook on-switch file is tracked under it.

## Findings

| path:line | severity | problem | fix |
|---|---|---|---|
| `scripts/tests/test_wave_queue.py:96-97` | Suggested (S-1) | The new case imports the test module `test_research_store_hook` to get `fake_store` and `warnings`. Running the queue file alone therefore imports the hook test module and its module-level imports (`test_wave_driver`, `research_wave`, ...), and a rename there would break this file. The hook module's autouse `fresh_hook` does not apply here. The case resets `_catalogs` itself and leaves `_registry` alone, which is harmless because `check` is faked. | Move `Ingests`, `catalog_on`, `fake_store` and `warnings` into a shared helper under `scripts/tests/` (or `wave_fixture.py`) and import them from both files. P10 or the next touch. |
| `scripts/wave_stage_record.py:349`, `:351`, `:360-364` | Suggested (S-2) | `record` also writes `wave-log.md` (class `wave-log`), `ledger-head.json` (class `ledger-head`) and the `record.copy_to` copies. The copy of `wave-result.json` matches class `wave-result`'s glob. None of these writes calls the hook, so a hook-fed catalog lacks them until `catalog --rebuild`. This is outside item 5's three named lines, and nothing moves in P9: SQL3-2 keeps the hook off, and G-P10 (c) builds from scratch. | When P10 turns the hook on, add the calls, or state the boundary in `research_store_hook`'s module doc ("the render classes' primary writes; copies and render-less side files by `catalog`"). |
| `.superpowers/sdd/platform-p9-20261003/task-SQL3-report.md:189-192` | Suggested (S-3) | Two G-P10 lines are stale for the freeze print. (e) still reads "PARTIAL until root adds `atx.run-store/v1`", but root registered it at `fd084e78`. (d) still reads "LANDED for C1's recording" (review 1 S5). | Root's G-P10 print should take (d) PARTIAL (SQL-6 recording landed; X-5 identities root's at the gate) and (e) landed for SQL3's literal. |
| review 1 S1-S8 (`task-SQL3-review.md`) | Suggested (S-4) | No item of review 1's eight Suggested findings was taken up in this round. SQL3-5-FINISH did not ask for them. S6 asked for one line each in this round's report (E2-4 not done; N1 / N2 done in effect), and none is there. | Carry: S1 (the F-1 strip at root's identity step) stays in root's merge checks; S5 / S6 are report lines; S2 / S3 / S7 / S8 are P10. |

## Evidence: my own runs vs the lane's logs

Run form for all of my runs:

- From `C:/atx-wt/pool-13`, each file in its own invocation.
- `"C:/Program Files/Python312/python.exe" -m pytest -p no:cacheprovider --basetemp <own scratch>/bt/<name>` (the
  parent was created first).
- Env: `PYTHONDONTWRITEBYTECODE=1`, `PYTHONHASHSEED=0`, `ATX_EQUITY_ROOT=C:/atx-wt/pool-2/build-equity`,
  `ATX_EQUITY_BIN=C:/atx-wt/pool-2/build-equity/bin`, and
  `ATX_RESEARCH_STORE_EXE=C:/atx-wt/pool-2/build-equity/bin/atx-research-store.exe`. The last one is needed because
  pool-13's own bin is deleted (SQL3-TREE), so without it the real-exe cases would skip.

**My own runs:**

- **Exes (EXE-CUR).** SHA-256 before, after the tests and after my teeth were identical:

  | exe | SHA-256 | build | used by the three files |
  |---|---|---|---|
  | `atx-research-store.exe` | `d8a500d84dc2a25130c69b226d8f5deec47b8dabb6256522c83c00764d3d9d65` | Debug `p9-2r`, source `fd084e78`; "current by source at `bba4c06f`" in root's `handoff-root-m5b.md` | yes, the only one |
  | `atx-research-eval.exe` | `47910191...` | `p9-2z` | no; hashed for comparison only |
  | `atx-equity-strategy-targets.exe` | `e6b21410...` | `p9-2y` | no; hashed for comparison only |
  | `atx-equity-strategy-ic.exe` | `ef3974cf...` | `p9-3b` | no; hashed for comparison only |

  - The IC exe differs from the lane's `fbb5b392...` (`p9-2y`): root relinked it at M5 (SIGN) after the lane's run.
  - I read nothing else under pool-2's `build-equity` and wrote nothing there.
- **On HEAD `0ebb83da`:**

  | file | result |
  |---|---|
  | `scripts/tests/test_research_store_hook.py` | exit 0, **13 passed** (`test_real_store_exe_ingests_every_class` PASSED: it ran) |
  | `atx-engine/tools/test_research_store_identity.py` | exit 0, **16 passed, 6 subtests passed** (`FixtureChain` PASSED: the real exe ran) |
  | `scripts/tests/test_wave_queue.py` | exit 0, **6 passed** (the new relative-root case PASSED) |

- **On the trial-merge tree `d6777122` (HEAD + root `bba4c06f`).** Root changed `scripts/tests/wave_fixture.py`,
  `test_wave_driver.py`, `wave_stage_library.py` and `wave_rules.py` after `175edd61`, and the fake wave imports
  them.
  - I ran a `git archive` of `scripts`, `atx-engine/{tools,schemas,tests/fixtures/research_store}`,
    `atx-impl/{tools,strategies}` and `.gitattributes` into my scratch, with the same run form.
  - Hook file **13 passed**, identity file **16 + 6 subtests passed**, queue file **6 passed**. The fake wave's
    ingest count is unchanged by root's edits.
- **Merges.** All three are plain (recomputed trees above).
- **Other checks.** The trial merge has no conflict. SQL3-2 holds. No literal was added. Porcelain was empty after
  every step. There were no permission refusals.

**From the lane's logs only:**

- The seven writers' files: 98 passed / 0 failed (`r11-writers-da19d7ac.log`, with the eval verb `p9-2z`).
- The R10 runs on `d91ce86c` and the WIP's predicted red (`r10-hook-wip-489d9a7e.log`).
- The lane's teeth 7 / 7.

## Teeth (my own; TEETH-RESTORE in the TEETH-CRLF form)

How the tool works:

- It is a scratch tool of mine, not a record. It mutates code under test only.
- Precondition: `git hash-object <rel>` = `git rev-parse HEAD:<rel>`, and porcelain is empty.
- It keeps the bytes in memory and as a copy, writes the mutation, runs the hook file, and restores in `finally`.
- Then it asserts: the raw SHA-256 of the working bytes = the kept copy; `hash-object` = HEAD's blob; porcelain is
  empty.
- It never uses `checkout`, `restore` or `stash`.

| # | mutation (code under test) | property | result |
|---|---|---|---|
| R1 | `wave_stage_record.py:348` doubled (the wave result is ingested twice) | exactly one ingest per write | **CAUGHT**, 2 failed / 11 passed: `test_one_ingest_per_write_in_a_fake_wave` (the ingest list has an extra entry) and `test_fake_wave_bytes_equal_with_a_failing_store` (wave-result warnings 2, expected 1). Restored: raw `e6d06cbb...` = kept; blob `2ee82fda...` = HEAD; porcelain `''`. |
| R2 | `cycle_verdict.py:196` moved above `path.write_text` (ingest before the write) | the ingest reads the whole file just written | **CAUGHT**, 3 failed / 10 passed: `[no-copy]` and `[keep-verdicts]` of `test_write_verdict_ingests_the_verdict_then_its_copy` (the bytes at ingest are absent), and `test_real_store_exe_ingests_every_class` (the real exe: "ingest exit 4 ... NotFound", one warning). Restored: raw `c595cb7e...` = kept; blob `19001b1c...` = HEAD; porcelain `''`. |

Final porcelain `''`; caught 2 / 2. These complement the lane's removals (M1-M4): a duplicate call and a mis-ordered
call are also caught.

## What root must check at merge (M6)

1. **Merge.** Merge `0ebb83da` (MERGE-SHA).
   - My trial `git merge-tree --write-tree 0ebb83da bba4c06f` is clean.
   - If root has moved past `bba4c06f`, recheck that only sprint-dir and Python paths conflict, if anything does.
2. **Python only.**
   - `git diff --name-only <root head>...0ebb83da` lists, outside `.superpowers/`, exactly the nine Python files
     above, with no C++ and no CMake. So there is no build.
   - EXE-CUR for the store exe: `p9-2r` `d8a500d8...`, current by source per `handoff-root-m5b.md`.
3. **The three test files** on the merge head, each in its own invocation, seed 0. Set `ATX_RESEARCH_STORE_EXE` to
   root's store exe; root's default bin path also works. Expect:
   - `scripts/tests/test_research_store_hook.py`: 13 passed, with the real-exe case RUN, not skipped.
   - `atx-engine/tools/test_research_store_identity.py`: 16 passed + 6 subtests, with `FixtureChain` RUN.
   - `scripts/tests/test_wave_queue.py`: 6 passed.
   - Plus the lane's seven writers' files (`test_cycle_resume.py`, `test_cycle_scoring.py`,
     `test_research_cycle_roles.py`, `test_research_wave.py`, `test_wave_driver.py`, `test_wave_hardening.py`,
     `test_wave_eval_verb.py`) with an `ATX_EQUITY_BIN` that holds the eval verb (ROOT-1a). Expect 0 failed.
   - Log the exe SHA-256 beside the runs.
4. **The hook's one-ingest count** (fake store; these must not drift on the merge head):
   - fake wave: 9 `stage-receipt` + 3 `wave-candidate` + 1 `wave-result` = 13 ingests, and 13 warnings with a failing
     store;
   - `write_binding`: 1;
   - `write_verdict`: 1, or 2 with `keep`;
   - `keep_copy`: 1 per copy;
   - the SQL3-WQ case: 3 (2 candidates + 1 manifest).

   Root's later edits to `wave_fixture.py` / `test_wave_driver.py` left 13 on my trial merge. A different count on
   root's head means a fixture change, to be attributed before merging.
5. **SQL3-2.**
   - `C:/atx-wt/pool-2/build-equity/research-store/` must not exist before or after root's runs. It was absent
     before and after every run of mine.
   - No test of these files writes there: the real-exe cases copy the exe into a temp root or use a temp catalog.
   - With the catalog off (all of P9), none of the six new call lines spawns a process or changes a byte.
6. **SQL2-CLS.**
   - The lane adds no `atx.*/vN` literal and does not touch `classes.json`. Root's own `classes.json` edit since
     `175edd61` (`atx.sign-receipt/v1`, SIGN-CLS-2) merges without conflict.
   - No classes-guard run is needed for this lane beyond the gate's.
7. **The freeze print.** Use G-P10 (d) / (e) as S-3 states, not the report's text. Keep review 1's F-1 strip for any
   console byte comparison (S-4).

## Needs a PM ruling

None.
