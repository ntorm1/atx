# Handoff SQL3 (P9 wave 3, pool-13): round R7-1 done at the owner stop; item 5 pending on B2

- Branch `feat/p9-sql3-20261004`, worktree `C:/atx-wt/pool-13`, lease run id `p9-sql3-20261004` (not touched).
- Root SHA merged: `1b64ce45` (= root's head at the owner stop; merges `93760a1e` of `a15ad887` and `9e44d3be` of
  `1b64ce45`, both clean, no conflict). Code commits: `d10d9e54` (items 1-4 + tests + pre-change evidence), `784d2abd`
  (F-2: the seal walk fails closed on a file-system error). Then the docs commit of this report / handoff.
- Builds: `p9-sql3-a` (source `d10d9e54`) and `p9-sql3-b` (source `784d2abd`), equity-dev, targets
  `atx-engine-research-catalog,atx-engine-research-catalog-tests,atx-research-store`; no first-party warning. Object
  tree `build-equity/` deleted by name at hand-back (receipts and logs `build-equity/mega-p9-sql3-{a,b}-*` kept, as
  are the tracked `build-equity/audits/`, `build-equity/v8-interim3-pitch-render-run2/`). Next free tag `p9-sql3-c`.
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
| 5 | three call lines in B2's files (`cycle_resume.write_binding`, `cycle_verdict.write_verdict` + `keep_copy`, `wave_stage_record.record`) | NOT DONE: pending, B2's redo not in root at the stop |

## Tests actually run (merged tree; ENV-1 did not occur)

- gtests `ResearchCatalog*:SealGuard*:StoreCli*`: 28 / 28 on `p9-sql3-a` (at `d10d9e54` and on the merged tree) and on
  `p9-sql3-b`.
- `scripts/tests/test_research_store_hook.py`: 8 passed (real exe of `-a`; again 8 passed with `-b`'s exe, seed 0).
- `scripts/tests` PYTHONHASHSEED 0: 520 passed, 4 skipped, 0 failed. PYTHONHASHSEED 1: SEE-SEED-1.
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
