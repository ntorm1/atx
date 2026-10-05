# Handoff C3 (lane C3, pool-15, branch `feat/p9-c3-20261004`, base `e1c16051`)

Contract: `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/resume4-dispatch.md`, `briefs/brief-C3.md`,
`wave3-carry.md` (C3 section, PM answers, "Added after drafting"). The report is `task-C3-report.md` (same dir).

## State

- Part A (C++): DONE; review `99b676f7` CHANGES_REQUIRED; **fix round 1 DONE_WITH_CONCERNS** (`9a7c490f` code +
  tests, then the docs commit; the round was stopped after build j and resumed by a fresh agent, no code changed on
  resume): R-1 (duplicate JSON keys refused), R-2 (narrower `leverage_range` refused with `construction_grid`), C3-3
  (`x-flag-aliases ["--cost-v2"]` on `cost-sqrt-v2` only), PRE-7 / PRE-7a (print-only `--ann-mean-gross`, NavSpec
  key `ann_mean_gross`), S-1, S-3 (as a recorded deviation, C3-6), S-4, S-5, S-7, part of S-6. Not done: the rest of
  S-6 (other lanes' comments, "on next touch"). Debug l and Release j: named 33 / 33, whole target-tests 390 / 390,
  canary 15 each; teeth k: six mutations, each caught by its own test. Concern: the explicit before / after exe
  comparison (pre-round vs post-round NAV, flag absent) was NOT run: the worktree switch to `99b676f7` it needed was
  refused by the permission system (report "Permission refusals"). Next: a scoped re-review of the fix round, then
  root's merge (report "Fix round 1 / Root at merge", summary below).
- Part B (C2-11 Python pm6-6 retirement, C2-9, C2-10, W3-6): PENDING, not started: it waits for E2 in root (C3-2)
  (root head `ab189471` = S2R + B2 identity part, merged into this branch at `9dace914`; no newer root merged).
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
- Fix round 1: p9-c3-f (Debug, fresh tree, 359 TUs, 443 s; canary 15; one red in a new test of mine, fixed),
  p9-c3-g (Debug: named 33/33, whole target-tests 390/390, canary 15), p9-c3-h (Release: the stale-PCH trap again,
  nothing tested), p9-c3-i (refused at memory admission, nothing built, no receipt), p9-c3-j (Release at clean
  `9a7c490f` after deleting the six `cmake_pch.cxx.{pch,obj}` by name, `CCACHE_RECACHE=1`: 274 TUs, 1082 s, canary
  15; named 33 / 33, whole 390 / 390 run on resume), p9-c3-k (Debug, test target only, six teeth mutations: 5 named
  tests fail as intended, 28 pass; reverted), p9-c3-l (Debug at `9a7c490f`, code clean: 12 TUs, canary 15, named
  33 / 33, whole 390 / 390).
- Next free tag: p9-c3-m. Build object trees: see "Disk" (a resumed round configures afresh; expect the stale-PCH
  trap on the first Release build: same remedy).

## Logs

`c3-logs/` here: gtest-a-named (18/18), gtest-a-whole, gtest-b-whole, gtest-c-whole (387/387 each), canary logs.
Fix round 1: `fix1-gtest-f-named.log` (32/33, the red fixed), `fix1-gtest-g-named.log` (33/33),
`fix1-gtest-g-whole.log` (390/390), `fix1-gtest-j-named.log` (33/33), `fix1-gtest-j-whole.log` (390/390, Release),
`fix1-gtest-l-named.log` (33/33), `fix1-gtest-l-whole.log` (390/390, Debug), `fix1-teeth-k-named.log` (5 intended
FAILs), `fix1-nav-identity.py` (tiny-world NAV driver) + `fix1-nav-optin-l.log` (opt-in tree check, PASS), canary
logs `mega-p9-c3-{f,g,j,l}-canary.log`. Receipts and build logs stay in `build-equity/` (`mega-p9-c3-*`).

## Disk

After the docs commit `5911a63e` the lane deleted by name the object trees of `build-equity` (CMakeFiles, atx-core,
atx-engine, atx-impl, atx-tsdb, bin, lib, tests and the tree files) and of `build-equity-rel` (same names), about
4.5 GB; kept: `build-equity/mega-p9-c3-*` receipts / build / cache-stats / canary logs, `c3-build-*.out`,
`c3-configure-*.log`, both `vcpkg-manifest-install.log`, and the two dirs that are not the lane's
(`build-equity/audits/`, `build-equity/v8-interim3-pitch-render-run2/`). Scratch `%TEMP%/c3-navid-l` deleted;
`%TEMP%/c3-navid-dry` (12 MB) is left: its deletion was part of the refused command.

## Root at merge (full list: report "Fix round 1 / Root at merge")

1. Scoped re-review of fix round 1, then merge; both presets, the nine targets of the report's item 1, `-Canary`;
   named filter 33 / 33, whole target-tests 390, `atx-impl-tests` once, canary 15 on the unchanged goldens.
2. X-5 and Y-F0: `nav <argv> --print-spec` (byte-preserving redirect, never PowerShell 5.1 `>`), then the argv run and
   the `nav --spec` run into two new dirs: every file byte-identical.
3. Release = Debug NAV on X-5 (G-P3 NAV half): equal except `summary.json` `producer` (`engine_git_sha`,
   `build_type`).
4. Flag-absent identity against the pre-C3 reference (without `--ann-mean-gross`): equal except
   `producer.engine_git_sha` (check the fields manifests for a `seal` block: without one stdout gains the P13 legacy
   line). The tiny-world before / after the lane could not run: `c3-logs/fix1-nav-identity.py old --bin <pre-merge
   bin>` then `new --bin <merged bin>`, `RESULT PASS`.
5. Wall time: the five research exes' build time on a fresh tag vs the pre-C3 receipt (F-10); X-5 NAV wall time
   before / after the 14-TU split, Debug and Release.
6. SQL2-CLS: `atx.nav-spec/v1` into `atx-engine/schemas/research_store/classes.json` (the only new schema literal of
   C3; this round adds none).
7. W3-8 kind-fixture flip (root post-merge, D3 owns the kind files): `new-cost-model.json` out of
   `SCHEMA_ONLY_FIXTURES`; run the 10 tiny-world kinds against the real `nav --list-rules --json`; D3's E2-8 guard
   reads `x-flag-aliases` (`--cost-v2` = `--cost-law cost-sqrt-v2`, ruling C3-3).
8. The opt-in (PRE-7a): flag `--ann-mean-gross`, NavSpec key `options.ann_mean_gross`; every P9 cell and its
   reference name it alike at the wave-3 P9-B0; with it only `summary.json` changes, by one `ann_mean_gross` key per
   scenario (a grid manifest's `summary_file_sha256` follows). New optional keys: scenario `ann_mean_gross` in
   `atx.dsl-nav-replay-summary/v1`, row `x-flag-aliases` in `atx.nav-rules/v1`.
9. `atx-impl-core` is INTERFACE: retarget any root list / append naming it; W1-I2 re-pin of lineage parents at P9-B0.

## Open questions

- The explicit before / after exe check (flag absent, pre-round vs post-round) is root's at merge (item 4) unless the
  owner allows the refused worktree switch (report "Permission refusals").
- Part B is pending by the dispatch rule, not by a question.
