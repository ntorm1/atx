# Handoff SQL3 (P9 wave 3, pool-13): items 1-4 merged in root; item 5 + SQL3-WQ DONE (R11: teeth 7 / 7, report, root `175edd61` merged at `da19d7ac`); ready for review 2 (BASE `69b8c162`)

## R11 round (SQL3-5-FINISH, Python only, no build; 2026-10-15)

- Head: the R11 docs commit (this file, `task-SQL3-report.md`, `sql3-evidence/r11-*`) over `da19d7ac` (merge of root
  `175edd61`, 0 conflicts; no file of this lane changed on root's side) over `00779453` (SQL3-WQ code + test) over
  `d43b3a8b` (the R10 handoff). Review 2: BASE `69b8c162`, file `task-SQL3-review-2.md` (FU-REVIEWS).
- DONE (1) SQL3-WQ: `scripts/wave_queue.py:146` hands the hook `Path(d or QUEUE_DIR) / p.name`; the emit line `:350`
  (same defect, same file: listed as a deviation) hands it `a.output`; test
  `scripts/tests/test_wave_queue.py:92` `test_a_relative_root_hands_the_store_hook_root_relative_names`.
- DONE (2) teeth: `sql3-evidence/r11-teeth-item5.py` (the R10 tool with `head_ok` in the TEETH-CRLF form: `git
  hash-object` vs `git rev-parse HEAD:<rel>`, raw SHA-256 vs the tool's own kept copy, porcelain empty; M6 / M7 added for
  SQL3-WQ), run once on `00779453`: caught 7 / 7, every restore check held, final porcelain empty
  (`r11-teeth-item5.log`).
- DONE (3) runs on `da19d7ac` (seed 0; root's exes read-only, SHA-256 before = after): hook file 13 passed (real-exe
  case ran, store `p9-2r` `d8a500d8...`); identity file 16 passed + 6 subtests; `test_wave_queue.py` 6 passed; the seven
  writers' files 98 passed / 0 failed (eval verb `p9-2z` `47910191...` via `ATX_EQUITY_BIN`). Logs
  `sql3-evidence/r11-{hook,identity,wave-queue,writers}-da19d7ac.log`, `r11-exe-sha-{before,after}.txt`.
- DONE (4) report: `task-SQL3-report.md` "Item 5 for the follow-up" (call lines, S5-1, SQL3-WQ, teeth table, runs,
  EXE-CUR, what root re-proves), the Outcome line, items row 5, G-P10 (b).
- (5) SQL3-TREE: the object tree in `C:/atx-wt/pool-13/build-equity` (`CMakeFiles/ atx-core/ atx-engine/ atx-impl/
  atx-tsdb/ bin/ lib/ tests/`, `.ninja_deps .ninja_log CMakeCache.txt CPackConfig.cmake CPackSourceConfig.cmake
  CTestTestfile.cmake build.ninja cmake_install.cmake compile_commands.json spdlog.pc`; 429,455,033 bytes measured) is
  deleted by name AFTER this commit; KEPT: `audits/`, `v8-interim3-pitch-render-run2/` (tracked), `mega-p9-sql3-{a,b}-*`,
  `vcpkg-manifest-install.log`. The result is in the hand-back.
- No permission refusal. No subagent. No build. Deferred to the gate (TEST-1): whole `scripts/tests` seeds 0 / 1, the
  stage chain, the rest of the store suite, the cross-commit tiny-world identity.

Next step: none for the lane; the PM dispatches review 2. Root merges at M6 iff APPROVE.

## R10 round (item 5, Python only, no build): state at OWNER STOP 10 (2026-10-08)

- Head `d91ce86c` (merge of root `5c811b56`, clean, no conflict; none of the six files below changed on root's side)
  over the code commit `b55c0d11` (over the WIP `489d9a7e`); then the docs commit of this file and the evidence.
  Tree clean at the stop. Review 2 BASE stays `69b8c162` (FU-REVIEWS).
- DONE, call lines (step 2 verified: resolver root attribute `Resolver.root`, `scripts/cycle/resolver.py:17`, unresolved
  `Path(root)`; `Wave.root`, `scripts/wave_context.py:36`, resolved; `import research_store_hook` in each file:
  `cycle_resume.py:38`, `cycle_verdict.py:30`, `wave_stage_record.py:13`; the classes `cycle-binding`, `cycle-verdict`,
  `wave-result` exist in `classes.json` naming these writers):
  `scripts/cycle_resume.py:193` (`write_binding`), `scripts/cycle_verdict.py:182-184` (`keep_copy`), `:196`
  (`write_verdict`), `scripts/wave_stage_record.py:348` (`record`, as the WIP had it).
- DONE, slip fixed in place (S5-1): the WIP handed the hook `cycle.res.path(...)` / `d / ...` / `path`, which are
  relative to the CWD when `--root` is relative (`scripts/cycle/cli.py:523` `type=Path`, `:568` `Resolver(a.root)`, never
  resolved), while the hook reads a relative path against the root (`research_store_hook.record`, `:150`): `--root r`
  would ingest `r/r/...` (one warning, the record not catalogued). The two cycle writers now pass the root-relative name
  they wrote. `wave_stage_record` unchanged (`Wave.root` is resolved).
- DONE, fakes without `.root` (cross-lane, minimal): `atx-engine/tools/test_research_store_identity.py:315`, `:353`
  (`root=self.cat.root`; that catalog is `tmp/catalog.sqlite`, outside the root: hook off);
  `atx-engine/tests/fixtures/research_store/make_store_tree.py:100` (`_Res.root = TREE`; no catalog / exe under the
  fixture tree: a no-op; generator NOT run, it rewrites the tracked fixture tree; read-only check
  `sql3-evidence/r10-make-store-tree-res-check.log`).
- DONE, tests in `scripts/tests/test_research_store_hook.py`: `Ingests.files` (:146, :151; the bytes the exe would read
  at the call: proves the call is after the write) and `Ingests.ingested` (:159); `fake_store` (:311);
  `test_one_ingest_per_write_in_a_fake_wave` (:321, +1 ingest `out/waves/w1/wave-result.json`, whole file at ingest);
  `test_fake_wave_bytes_equal_with_a_failing_store` (:341, 13 warnings incl. 1 wave-result); new
  `test_write_binding_ingests_its_binding[absolute-root|relative-root]` (:396),
  `test_write_verdict_ingests_the_verdict_then_its_copy[no-copy|keep-verdicts]` (:410),
  `test_keep_copy_ingests_each_copy` (:422); `test_real_store_exe_ingests_every_class` (:467) now runs the writers with
  their own calls and a real `Resolver` (the duplicate explicit `H.record` calls removed).
- DONE, runs (TEST-1, seed 0, `-p no:cacheprovider`, own basetemp, `PYTHONDONTWRITEBYTECODE=1`; logs in `sql3-evidence/`):
  WIP `489d9a7e` as committed: hook file 3 failed / 5 passed, as the WIP body predicted (`r10-hook-wip-489d9a7e.log`).
  Code `b55c0d11` before the merge: hook file 13 passed (`r10-hook-dev1.log`); identity file 16 passed + 6 subtests
  (`r10-identity-dev1.log`); the writers' own files (`test_cycle_resume.py`, `test_cycle_scoring.py`,
  `test_research_cycle_roles.py`, `test_research_wave.py`, `test_wave_driver.py`, `test_wave_hardening.py`,
  `test_wave_eval_verb.py`) 95 passed / 4 failed (`r10-writers-dev1.log`): each of the 4 is
  `nav_summ: no engine statistics executable C:\atx-wt\pool-13\build-equity\bin\atx-research-eval.exe` (pool-13 never
  built that target; env, not item 5); the same 4 with `ATX_EQUITY_BIN=C:/atx-wt/pool-2/build-equity/bin` (root's eval
  verb `p9-2v` `6962966c...`, read-only): 4 passed (`r10-writers-dev1-eval-bin.log`).
  Merged head `d91ce86c`: hook file 13 passed, real-exe case RAN with root's store exe `p9-2r` `d8a500d8...` (copied into
  temp roots, read-only) (`r10-hook-final-d91ce86c.log`); identity file 16 passed + 6 subtests, its real-exe case ran
  (`r10-identity-final-d91ce86c.log`); the writers' seven files with root's bin 98 passed / 0 failed
  (`r10-writers-final-d91ce86c.log`; 98 vs 99 before: root's C3b merge moved wave cases out of `test_wave_driver.py`).
  Deferred to the gate (TEST-1): whole `scripts/tests` seeds 0 / 1, stage chain (not edited), the rest of the store
  suite, the tiny-world flag-absent identity.
- NOT DONE (1) teeth: tool `sql3-evidence/r10-teeth-item5.py` (M1-M4 one call line removed each, M5 the WIP form put
  back in `write_binding`; expected reds listed in the tool). Its one run on `d91ce86c` STOPPED at the precondition
  before any mutation (`STOP before M1 ... sha256 df1b65e4... HEAD blob 95998548...`; nothing written; porcelain empty
  before and after): `core.autocrlf=true`, the blobs are LF and the working copies CRLF, so `sha256(file)` never equals
  `sha256(git cat-file blob HEAD:path)`. Fix (one function, `head_ok`): compare `git hash-object <rel>` with
  `git rev-parse HEAD:<rel>` (checked: equal for the three files), or hash `git cat-file --filters HEAD:<rel>` (checked:
  equals the working bytes). Then run it once (command below); expected `caught 5 / 5`.
- NOT DONE (2) report: section "Item 5 for the follow-up" of `task-SQL3-report.md` (fill from this section; row 5 of the
  items table, the Outcome line and G-P10 (b) updated). (3) SQL3-TREE: delete by name, AFTER the report commit, the
  object tree in `C:/atx-wt/pool-13/build-equity` (`CMakeFiles/ atx-core/ atx-engine/ atx-impl/ atx-tsdb/ bin/ lib/
  tests/`, `.ninja_deps .ninja_log CMakeCache.txt CPackConfig.cmake CPackSourceConfig.cmake CTestTestfile.cmake
  build.ninja cmake_install.cmake compile_commands.json spdlog.pc`; about 410 MB); KEEP `audits/`,
  `v8-interim3-pitch-render-run2/` (tracked), `mega-p9-sql3-{a,b}-*`, `vcpkg-manifest-install.log`.
- `sql3-evidence/i5-fakewave-pre*.txt` / `.err`: the STOP 9 agent's catalog-off digest capture at `2d02466e` with a
  variant of `sql3-fakewave-digests.py` (a `raw=` column; root path not recorded): committed as found, not used as
  evidence (the cross-commit identity is the gate's, TEST-1).
- Open: `scripts/wave_queue.py:146` (item 1, approved) has the same CWD-relative pattern: `--root` (`:275`) is never
  resolved (`:303`), so a relative `--root` other than `.` ingests `<root>/<root>/...` (one warning, no byte change);
  not edited (outside item 5).
- No permission refusal this round. No subagent. No build.

Exact next step for a fresh agent: (1) fix `head_ok` in `r10-teeth-item5.py` as above; run, from `C:/atx-wt/pool-13`,
`PYTHONDONTWRITEBYTECODE=1 ATX_RESEARCH_STORE_EXE=C:/atx-wt/pool-2/build-equity/bin/atx-research-store.exe
"C:/Program Files/Python312/python.exe" -B .superpowers/sdd/platform-p9-20261003/sql3-evidence/r10-teeth-item5.py
C:/atx-wt/pool-13 <own basetemp parent> .superpowers/sdd/platform-p9-20261003/sql3-evidence/r10-teeth-item5.log`
(a STOP exit 2 after a mutation = frozen tree: report, commit nothing); (2) `git merge feat/platform-p9-20261003` if
root moved in code, re-run the hook file only if a merged file touches the hook or the three writers; (3) the report
section; this head line; commit (`git add -f`); (4) SQL3-TREE deletion by name; hand back for review 2 (BASE `69b8c162`).

- Branch `feat/p9-sql3-20261004`, worktree `C:/atx-wt/pool-13`, lease run id `p9-sql3-20261004` (not touched).
- Root SHA merged: `1b64ce45` (= root's head at the owner stop; merges `93760a1e` of `a15ad887` and `9e44d3be` of
  `1b64ce45`, both clean, no conflict). Code commits: `d10d9e54` (items 1-4 + tests + pre-change evidence), `784d2abd`
  (F-2: the seal walk fails closed on a file-system error). Then the docs commit of this report / handoff.
- Builds: `p9-sql3-a` (source `d10d9e54`) and `p9-sql3-b` (source `784d2abd`), equity-dev, targets
  `atx-engine-research-catalog,atx-engine-research-catalog-tests,atx-research-store`; no first-party warning. Object
  tree `build-equity/`: NOT deleted (the R7 text said "deleted by name at hand-back" in error; R8 check: about 410 MB
  present, `bin/` holds `p9-sql3-b`'s exes `466fcde5...` / `257f55a8...`, which the real-exe hook test uses).
  Receipts and logs `build-equity/mega-p9-sql3-{a,b}-*` and the tracked `build-equity/audits/`,
  `build-equity/v8-interim3-pitch-render-run2/` are kept. Deleting the object tree by name is left to the lane's close
  (after item 5). Next free tag `p9-sql3-c`.
- R8 round (close; no build): DIRTY-1 commit `f48d0585` (this file as it stood at the stop); `git merge
  feat/platform-p9-20261003` = already up to date (root still `1b64ce45`; B2's redo not in root); SQL3-SEED: the whole
  `scripts/tests` under PYTHONHASHSEED 1 on head `f48d0585` (code = `784d2abd`), counts below.
- Full record: `task-SQL3-report.md` (file:line per item, tests, evidence logs in `sql3-evidence/`, SQL3-1, the
  tiny-world identity procedure for root, G-P10 (a)-(e), findings F-1 / F-2, permission refusals).

## Items (file:line at `784d2abd`)

| # | item | status |
|---|---|---|
| 1 | hook `scripts/research_store_hook.py` (`selection` :126, `record` :139, `ingest` :162, `check` :195); runner `scripts/run_bounded_research.py:491-494`, `:557-562` (`store` block last in start / receipt, hook after each write, enabled only by the recorded block); `atx-engine/tools/stage_chain.py:118-135`, `:282`, `:286` (`on_write`); `scripts/wave_queue.py:146`, `:350` | DONE |
| 2 | `scripts/research_wave.py:26`, `:41` (`chain_of` passes the hook, class `stage-receipt`) | DONE |
| 3 | W3-4 `atx-engine/src/research/store/catalog/seal_guard.cpp:40-79`, `:162-176` (outermost `.git` ancestor; fs error = whole path); `seal_guard.hpp:9-14`, `:58-63`; gtest `research_catalog_seal_test.cpp:84` `SealGuard.RootTokenScannedOnlyBelowTheRepoTop`; N3 documented (hook doc; real-exe test) | DONE |
| 4 | W3-5 `atx-impl/tools/book_monitor.py:242-246` (refusal), `:250-257` (`work_indexes`); test `test_book_monitor.py:321` | DONE |
| SQL3-1 | literal `atx.run-store/v1`; proposed rows: `"registers": ["atx.run-store/v1"]` on `run-receipt` and on `run-start` in `atx-engine/schemas/research_store/classes.json` (NOT edited here; checked in memory: guard green, `shared_literal_problems` []) | LISTED for root; the classes guard is the expected red naming only `atx.run-store/v1` |
| 5 | three call lines in B2's files (`cycle_resume.write_binding`, `cycle_verdict.write_verdict` + `keep_copy`, `wave_stage_record.record`) | R10: coded, tested, merged with root (`b55c0d11`, `d91ce86c`); teeth / report / tree deletion left (section "R10 round" above) |

## Tests actually run (merged tree; ENV-1 did not occur)

- gtests `ResearchCatalog*:SealGuard*:StoreCli*`: 28 / 28 on `p9-sql3-a` (at `d10d9e54` and on the merged tree) and on
  `p9-sql3-b`.
- `scripts/tests/test_research_store_hook.py`: 8 passed (real exe of `-a`; again 8 passed with `-b`'s exe, seed 0).
- `scripts/tests` PYTHONHASHSEED 0: 520 passed, 4 skipped, 0 failed. PYTHONHASHSEED 1 (R8, head `f48d0585`, exe of
  `p9-sql3-b`, real-exe case ran): 520 passed, 4 skipped, 0 failed, 27 min, ENV-1 did not occur
  (`sql3-evidence/pytest-scripts-tests-seed1-r8.log`; the 4 skips are the env-gated live / e2e cases, as at seed 0).
- `atx-engine/tools/test_stage_chain.py` seeds 0 / 1: 10 / 10 passed.
- Store suite (`test_research_store_{classes,blind,identity,fixtures}.py`, `test_research_store.py`) seeds 0 / 1:
  42 passed, 1 failed each = the classes guard (expected, `atx.run-store/v1` only).
- `atx-impl/tools/test_book_monitor.py`: 13 passed.
- Tiny-world fake-wave digests: pre-change = post-change = merged, catalog off (69 files identical).

## Next step for a fresh agent (item 5, only after B2's redo is merged in root)

1. `git merge feat/platform-p9-20261003`.
2. Add, each right after its write (and `import research_store_hook` in each file):
   `cycle_resume.write_binding` -> `research_store_hook.record(<resolver root>, <binding path>, "cycle-binding")`;
   `cycle_verdict.write_verdict` -> `record(<root>, path, "cycle-verdict")`; `keep_copy` -> `record(<root>,
   d / f"{mode}-{k}.json", "cycle-verdict")` before its `return`; `wave_stage_record.record` -> after
   `w.write_json(path, doc)`: `record(w.root, w.path(path), "wave-result")`. Check the resolver's root attribute name
   (`cycle.res.root` at `5c57c1ef`).
3. Tests: extend `test_one_ingest_per_write_in_a_fake_wave` (+1 ingest: the wave result) and a fake-store unit case per
   writer; the real-exe case already runs these three writers followed by the same call.
4. No C++: no build. Suites: `scripts/tests` under seeds 0 and 1, the stage chain, the store suite; report in
   `task-SQL3-report.md` ("Item 5 for the follow-up").
