# Handoff C3 (lane C3, pool-15, branch `feat/p9-c3-20261004`, base `e1c16051`)

Contract: `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/resume4-dispatch.md`, `briefs/brief-C3.md`,
`wave3-carry.md` (C3 section, PM answers, "Added after drafting"). The report is `task-C3-report.md` (same dir).

## State

- Part A (C++): DONE. Part B (C2-11 Python pm6-6 retirement, C2-9, C2-10, W3-6): PENDING: root had no E2 merge
  (root head `ab189471` = S2R + B2 identity part, merged into this branch at `9dace914`).
- Commits: `4e958185` wip(c3) functional work; `7e4c9dc4` move-only split (alone); `9dace914` merge root
  `ab189471` (CMake tail conflict: B2's ledger link -> atx-impl-pipeline); then the CMake guard commit
  (`atx-impl-core` INTERFACE takes no sources), then the report commit.
- Build trees: `build-equity` (equity-dev), `build-equity-rel` (equity-rel). Both are regenerable: delete by name
  after the report (keep `build-equity/*receipt*`, `*-build.log`, `*-canary.log`, `c3-*.log`).

## Part B (next round, on a root head that contains E2's merge)

1. `git merge <root head>` (never rebase). Read root's handoff "Concern: C2 item (ii) is C3's"
   (`handoff-root-m2e2.md`) and ruling C2-11 (progress.md).
2. C2-11: retire `scripts/wave_rules.py` `matched_leverage` / `gross_matches` and their users:
   `wave_stage_cell.match` (`gross_match: pm6-6` path and `none`), `FakeCycle.calibrate` (`wave_fixture.py`), the
   fixture default manifest `"gross_match": "pm6-6"` -> `"pm6-6-engine"`, the direct asserts in
   `test_research_wave.py`, replace the engine-vs-Python test by a pinned-fixture test of the engine route, retire the
   G-P5 row in `test_no_python_mirror.py`. Line numbers move with E2: re-locate by symbol.
3. C2-9: a manifest that needs gross matching and does not name `gross_match` is refused at plan, the message naming
   the key and the value `pm6-6-engine`.
4. C2-10: digest re-check of the cell of record immediately before `wave_result.build`.
5. W3-6: list the PM7-35 copy (`wave_rules.py:50-51`) for root; delete only after root's identity vs B1's predicate.
6. Pytest: explicit paths, `scripts/tests` and `atx-engine/tools` in separate invocations, `-p no:cacheprovider`,
   own `--basetemp`, `PYTHONDONTWRITEBYTECODE=1`.

## Build tags used

- p9-c3-a (Debug): target-tests, targets exe. 577 s, 328 TUs (cold tree).
- p9-c3-b (Debug, -Canary): + IC exe, after the split. Canary 15 passed.
- p9-c3-c (Debug, -Canary): merged head; + atx-research-eval, the five research exes, atx-impl. Canary 15 passed.
- p9-c3-d (Release, -Canary): refused by the stale-PCH trap (ccache PCH vs the fresh `deps/equity-rel`).
- p9-c3-e (Release, -Canary, `CCACHE_RECACHE=1` after deleting the six `cmake_pch.cxx.{pch,obj}` by name):
  776 s, 299 TUs; target-tests 387/387; canary 15 passed.
- Next free tag: p9-c3-f. Build trees deleted after the report (a resumed round configures afresh; expect the
  stale-PCH trap on the first Release build: same remedy).

## Logs

`c3-logs/` here: gtest-a-named (18/18), gtest-a-whole, gtest-b-whole, gtest-c-whole (387/387 each), canary logs.

## Open questions

- none (part B is pending by the dispatch rule, not by a question)
