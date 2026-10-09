# P9 wave-3 merges: root merge report

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. One section per wave-3 lane merged, in merge order
(`r7-root-merge.md`; B2 and T2 go in `root-wave2-merge-report.md`). Summary rows: `integration-log.md` "P9 wave-3
merges". Zero trials throughout; no statistic is read or recorded here.

## SQL3

Step group M1 of RESUME 8 (ledger line "Dispatch root M1"), 2026-10-06. Evidence: `m1-logs/` (tools `m1-logs/tools/`:
`build.sh`, `gt.sh`, `pyt.sh`, `fixture_chain.sh` copied from M2j / M2f with the logs dir changed; `fakewave_f1.py` =
the lane's `sql3-fakewave-digests.py` plus the F-1 filter). Handoff: `handoff-root-m1.md`.

### Start state

- Docs head `1b64ce45` (code `ebc918c3`), as `handoff-root-m2j-fin.md` left it. Step 0 housekeeping docs commit
  `cfd5a25d` (`progress.md`, `next-goal-prompt.md`, `r7-dispatch.md` as they stood; `git add -f` of `status-8.md`,
  `r7-review-dispatch.md`, `r7-root-merge.md`, `resume8-addendum.md`, `r8-dispatch.md`, `r8-review-notes.md`,
  `root-gate-brief.md`, `data-classification.md`, `briefs/brief-{ABS,SIGN,D3W-PB}.md`). Not added: `p9-prereg.md`
  (modified: the PRE-2b-i agent's), the four `docs/plans/*.png`, `disk-audit.md` (untracked, not in the list).
- Tree otherwise clean. Trial ledger `build-equity/trials.jsonl` 133 lines, sha256 prefix `27e40f9f` (start and end).
  C: 50 GB free.

### Merge

- Lane head `a02c074f` (pool-13, `feat/p9-sql3-20261004`); `Task SQL3: APPROVE at a02c074f` (review `69b8c162`,
  `task-SQL3-review.md` in pool-13: 0 Required, 8 Suggested; BASE `1b64ce45` = the merge base).
- `git merge --no-ff a02c074f` -> **`fbd2c045`**, **0 conflicts** (root had moved only by the docs commit `cfd5a25d`).
  Merge tree `8fee24e4` = `git merge-tree --write-tree cfd5a25d a02c074f`; outside `.superpowers/` the merged tree
  equals the lane's `a02c074f` (0 files differ). The lane's records arrived with the merge (`task-SQL3-report.md`,
  `handoff-SQL3.md`, `sql3-evidence/`); the review file sits in the lane's docs commit `69b8c162` after the approved
  SHA and stays in pool-13 (merged by SHA, as the contract says).
- Code files (12): `seal_guard.{hpp,cpp}`, `research_catalog_seal_test.cpp`, `stage_chain.py`, `test_stage_chain.py`,
  `book_monitor.py`, `test_book_monitor.py`, `scripts/research_store_hook.py` (new), `scripts/research_wave.py`,
  `scripts/run_bounded_research.py`, `scripts/tests/test_research_store_hook.py` (new), `scripts/wave_queue.py`.

### SQL2-CLS (SQL3-1): **`fd084e78`**

- `atx-engine/schemas/research_store/classes.json`: `"registers": ["atx.run-store/v1"]` added as the LAST key of the
  `run-receipt` row (glob `**/receipt.json`) and of the `run-start` row (glob `**/start.json`), after `"stage": 1`, as
  the `wave-reader` row. Line endings unchanged (`i/lf w/lf`).
- Classes guard (`test_research_store_classes.py`): before **1 failed / 4 passed**, the only unregistered literal
  `atx.run-store/v1` (in `scripts/research_store_hook.py`, `scripts/run_bounded_research.py`); after **5 passed**.
- Store suite (`test_research_store_{classes,blind,identity,fixtures}.py`, `test_research_store.py`; run after the
  Debug build so the identity case uses the merged exe): **43 passed, 0 skipped** under `PYTHONHASHSEED` 0 and 1.
- Fixture chain (`m1-logs/tools/fixture_chain.sh`, store exe p9-2r `d8a500d8`): rel and abs `files_seen 28 verified 25
  declared 0 skipped 4`, **`catalog_digest d32f7655...4e69` unchanged**, `verify --pins` 0, identity 12 checked /
  0 mismatches, rel = abs.

### EXE-CUR builds

`scripts/research-build.ps1` through `m1-logs/tools/build.sh` (waits for no other build on the host and the memory
admission; one build at a time; no `-Canary`: no NAV / IC exe built). Targets exactly
`atx-engine-research-catalog,atx-engine-research-catalog-tests,atx-research-store`; source **`fd084e78`** for both
(DirtyEntries 5 = `p9-prereg.md` + the four PNGs; no code).

| tag | preset | exit | TUs | warning lines | `atx-research-store.exe` | `atx-engine-research-catalog-tests.exe` |
|---|---|---|---|---|---|---|
| **p9-2r** | equity-dev (`build-equity/bin`) | 0 | 14 | 0 | `d8a500d84dc2a25130c69b226d8f5deec47b8dabb6256522c83c00764d3d9d65` | `f1134aaa72338346fcb642ddd57ff15fe6e59cdeb078f1c20782dca45c0f78ed` |
| **p9-2s** | equity-rel (`build-equity-rel/bin`) | 0 | 25 | 0 | `0d6083af4083d7f613956a6480ef3a9b3570c95cf929c8163124b018365e4770` | `712907c98c3090f6faa5284a6752aecc6eddcded118bbc901b140ca9b8d3b192` |

p9-2r waited about one minute at the gate (another build on the host). On-disk SHA-256 = receipts
(`exe-sha256.txt`). EXE-CUR for the rest: `git diff --name-only 327c86b2..fd084e78` over C++ / CMake / presets =
`seal_guard.{hpp,cpp}` and `research_catalog_seal_test.cpp` only; `seal_guard.hpp` is included only by
catalog-library TUs, and only `atx-research-store` and the catalog tests link `atx::engine-research-catalog`, so the
nine p9-2p / p9-2q exes of `handoff-root-m2j-fin.md` stay current.

### Tests (all on the merged head; ENV-1: 0 hits in every log)

| suite | invocation | result |
|---|---|---|
| gtests Debug p9-2r | `atx-engine-research-catalog-tests.exe --gtest_filter=ResearchCatalog*:SealGuard*:StoreCli*` | **28 passed**, 0 failed (`dbg-gt-catalog.log`) |
| gtests Release p9-2s | same | **28 passed**, 0 failed (`rel-gt-catalog.log`) |
| hook file, real exe | `pytest -rs scripts/tests/test_research_store_hook.py` (exe p9-2r) | **8 passed**, 0 skipped (the real-exe case ran) |
| book monitor | `pytest atx-impl/tools/test_book_monitor.py` | **13 passed** |
| ROOT-1 `scripts/tests` seed 0 | `pytest -rs scripts/tests`, `PYTHONHASHSEED=0` | @@S0@@ |
| ROOT-1 `scripts/tests` seed 1 | same, `PYTHONHASHSEED=1` | @@S1@@ |
| ROOT-1 stage chain seeds 0 / 1 | `pytest atx-engine/tools/test_stage_chain.py` (own invocations) | **10 passed** each |
| store suite seeds 0 / 1 | as SQL2-CLS above | **43 passed** each |

All pytest: `C:/Program Files/Python312/python.exe`, explicit paths, `-p no:cacheprovider`, own `--basetemp`
(session scratchpad `root-m1/bt/`), `PYTHONDONTWRITEBYTECODE=1`. Known-red set: **EMPTY** (expected and found).

Slip (logged in `pyt-exits.txt`): the first stage-chain pair errored at setup (10 errors each) because the
`--basetemp` parent did not exist (pytest creates only the leaf); void, re-run with the parent created: 10 passed each.

### Root items

| item | source | result |
|---|---|---|
| 1 SQL3-1 rows (SQL2-CLS) | review "root must check" 1 | **holds**: `fd084e78`; guard 1 failed -> 5 passed; store suite 43 x seeds 0 / 1; fixture chain digest unchanged |
| 2 build before testing | review 2, EXE-CUR | **holds**: p9-2r / p9-2s built from `fd084e78` before the store suite, the hook file and `scripts/tests`; gtests 28 on both |
| 3 tiny-world identity, catalog off (report "For root" 1-2, F-1 filter) | report, review 3 / S1 | **holds** (below) |
| 4 hook on-switch in pool-2 (SQL3-2) | review 4, ruling SQL3-2 | **holds, hook OFF**: `build-equity/research-store/catalog.sqlite` (and its dir) does NOT exist in pool-2; 0 of 1,473 `receipt.json` / `start.json` under `build-equity/`, `build-equity-rel/` carry `atx.run-store/v1` (string match, nothing parsed; `sql3-2-check.txt`) |
| 5 `index.sqlite` under a cell's argv dirs before the first real run | review 5 | **carried** to the root that runs P9-B0 / the cells (no real run in M1) |
| 6 item 5 (B2's files) | report, review 6, B2-12 | **pending by rule**: B2's redo is not in root |

Item 3 detail:

- Step 1 (fake wave, `m1-logs/tools/fakewave_f1.py`, one fixed root `scratchpad/m1-fw/root`, same repo path):
  pre-merge head `cfd5a25d` (code `ebc918c3`) and code head `fd084e78`: both exit 0, **69 / 69 files identical**
  after the normalisation (JSON: canonical `time_free`; other files: git's "warning: in the working copy of " lines
  dropped). Raw bytes: the 9 stage receipts differ (time keys only: their time-free JSON is equal); the commit
  consoles were raw-equal in this pair, so the F-1 filter changed no verdict here (`fw-cmp.txt`).
- Step 2 (runner, no catalog, no index: `run_bounded_research.py --root <scratch> --no-git --seconds 60
  --max-rss-mib 512 --min-free-mib 64 -- python -c pass`, same root both heads): `start.json` 16 keys and
  `receipt.json` 20 keys, same keys in the same order, **no `store` key**, equal outside the report's substitution
  list (`started_utc`, `minimum_system_free_bytes`, and in the receipt also `sampled_peak_tree_rss_bytes`,
  `wall_seconds`, `owned_processes` differ; `runner-cmp.txt`).
- Step 3 (catalog on) is not run by root: under SQL3-2 the on-path is proven by SQL3's tiny-world tests and the
  reviewer's run only; root's run of the hook file above includes the real-exe case on p9-2r's store exe (8 passed).

### G-P10 (a)-(e) as the report claims (printed, gates nothing, SQL-8)

- (a) pins indexed / `verify --pins` counts: **P10 for SQL3 (the gate's store step)**: not SQL3's work (the counts are
  root's gate store step, M2A-4, not run yet). SQL3's W3-4 narrowing is LANDED (gtest
  `SealGuard.RootTokenScannedOnlyBelowTheRepoTop` green on both presets) and removes a host-environment false refusal;
  not re-measured.
- (b) render identity of the seven record classes: **PARTIAL**: run-start, run-receipt, stage-receipt and
  wave-candidate are re-rendered at write time (wave-manifest presence-checked); binding, verdict (+ copy) and wave
  result wait on item 5 (B2); the real-exe case re-renders all with 0 mismatches (8 passed here).
- (c) two from-scratch catalog digests: **the gate's** (root's store step, at a non-default `--catalog` path per
  SQL3-2); unchanged by SQL3.
- (d) caches: **PARTIAL** (the review's S5 correction of the report's "LANDED"): the SQL-6 recording (receipt `store`
  block) LANDED; the X-5 cache identities are root's at the gate; SQL-11 holds; `book_monitor --fit-work` refuses an
  indexed work dir (W3-5, 13 passed).
- (e) every `atx.*/vN` literal registered: **LANDED** for every literal in root at `fd084e78` (guard 5 passed); later
  lanes' literals come with their merges (SQL2-CLS).

### Review findings not closed by root (Suggested; none blocks)

S1 applied in root's identity tool (the F-1 filter). S5 applied in the tick above. S2 (an F-2 seam + gtest) and S3
(bound exhaustion returns nullopt) are C++ in `seal_guard.cpp`: P10 or a later C++ round. S4 (stale comment
`catalog.hpp:27`, SQL2's file, "root or P10"): left for P10 / the next lane that touches the catalog header (a comment
edit there makes every catalog TU and both exes stale under EXE-CUR for no behaviour). S6, S7, S8: the item-5 round
or P10.

### For PRE (round 2b-ii facts from SQL3)

None. No Appendix IV placeholder of `p9-prereg.md` names SQL3. One fact for the text, if it wants it: under SQL3-2 the
record hook is OFF for every run of record (default catalog path absent at `fd084e78`; no receipt carries a `store`
block), so P9-B0 references and cells need no `store` entry on a substitution list.

### Disk and status

- pool-13's `build-equity/` object tree (about 410 MB) **kept**: SQL3-TREE keeps it through the item-5 follow-up
  round (its exes run the real-exe hook test there; deleting it now would turn that case into a skip or force a
  rebuild at the memory gate); C: has 50 GB free. To delete by name after the item-5 round (DISK-2): `CMakeFiles/`,
  `atx-core/`, `atx-engine/`, `atx-impl/`, `atx-tsdb/`, `bin/`, `lib/`, `tests/`, `.ninja_deps`, `.ninja_log`,
  `CMakeCache.txt`, `CPackConfig.cmake`, `CPackSourceConfig.cmake`, `CTestTestfile.cmake`, `build.ninja`,
  `cmake_install.cmake`, `compile_commands.json`, `spdlog.pc`; keep `mega-p9-sql3-*`, `vcpkg-manifest-install.log`,
  the tracked `audits/` and `v8-interim3-pitch-render-run2/`.
- Root's own object trees kept (disk above 12 GB; the next root group reuses them). Scratch under the session
  scratchpad (`m1-fw/`, `m1-run/`, `root-m1/`, `m1-merge-msg.txt`): deleted by name at the end.
- Code head **`fd084e78`**. Build tags used p9-2r, p9-2s; **next free build tag p9-2t**. Known-red set: **EMPTY**.
  Trial ledger 133 lines `27e40f9f` (0 trials). No lease touched. No permission refusal.

## Step group M3 (RESUME 10): C3b, D3W, ABS

Root M3, 2026-10-08 / 09 (ruling ROOT-M3-SCOPE; dispatch `r10-dispatch.md` "ROOT M3", ledger "Dispatch root M3").
Evidence: `m3-logs/` (tools `m3-logs/tools/`: `build.sh`, `gt.sh`, `pyt.sh`, `fixture_chain.sh` copied from
`m1-logs/tools` with the logs dir changed and the wait bound raised to 180 minutes (HOST-4); `busy.ps1` (HOST-5:
foreign builds outside `C:\atx-wt\` ignored); `warn_count.py` (the build log is UTF-16); `keep_both.py` (B2-EVAL-KEEP /
B2-14 hunk check)). Exit tables `m3-logs/gt-exits.txt`, `m3-logs/pyt-exits.txt`, `m3-logs/builds.txt`. Handoff:
`handoff-root-m3b.md`. TEST-1 / ROOT-1a throughout: targeted checks at each merge; whole suites, Release, canaries,
golden and the parents' flag-absent identity runs are deferred to the gate (TEST-1).

Start state: docs head `baefd8a9` (code `fcb0200e`); tracked changes only the PM's `progress.md` and
`next-goal-prompt.md`; the five `docs/plans/*.png` untracked (not root's); trial ledger 133 lines `27e40f9f`;
`build-equity/research-store/` absent (SQL3-2). Step 0 housekeeping docs commit **`8346fc2e`** (`progress.md`,
`next-goal-prompt.md`; `git add -f` `status-10.md`, `handoff-root-m1.md`, `root-wave3-merge-report.md`,
`disk-audit.md`, `resume10-addendum.md`, `r10-dispatch.md`; no PNG, no cache dir).

## C3b

### Merge

- `git merge --no-ff 76b9f256` (review APPROVE of `b2517fe4`; MERGE-SHA) -> **`b182dfdd`**, **0 conflicts**; tree
  `49159e4f` = the `git merge-tree --write-tree` recompute. Merge base `685e238b`.
- 27 code files (+590 / -176): C++ `atx-impl/src/book/nav_run.cpp`, `nav_spec.cpp`, `nav_spec.hpp`,
  `atx-impl/tests/strategy_nav_spec_test.cpp`; Python `scripts/wave_{manifest,result,rules,stage_cell,stage_preflight}.py`;
  the eight kind fixtures `scripts/specs/p9/kinds/*.json` (line 18, `pm6-6-engine`); tests `test_wave_gross_match.py`
  (new), `test_research_wave.py`, `test_wave_driver.py`, `test_wave_review.py`, `test_wave_review_minor.py`,
  `test_wave_scoreboard.py`, `test_wave_speed.py`, `test_no_python_mirror.py`, `test_wave_kinds.py`, `wave_fixture.py`.
  The lane's records (`handoff-C3.md`, `task-C3-report.md`, `task-C3b-review.md`, `c3-logs/`) arrived with the merge.
- Keep-both with B2 (merged first, M2): the three files both sides touch auto-merged and every hunk of both survives
  (`m3-logs/c3b-keep-both.txt`, `keep_both.py`): `test_no_python_mirror.py` (B2 1 hunk, C3b 1), `test_wave_kinds.py`
  (B2 1 hunk: `F.pin_eval_verb(root)` in `world()` at merged `:171`; C3b 3 hunks at `:613-614`, `:733`, `:759`),
  `wave_fixture.py` (B2 2, C3b 4).

### Build (EXE-CUR)

| tag | preset | targets | exit | TUs / links | wall | warning lines | exe SHA-256 (source `b182dfdd`) |
|---|---|---|---|---|---|---|---|
| **p9-2u** | equity-dev | `atx-impl-strategy-target-tests` | 0 | 16 / 2 | 125 s | 0 | `7a427e2c5948ab45e06f7f4ff0aed0995694d71dc117be5e1326056c37e27a7a` |

The named filter needs no NAV exe (the NavSpec cases run in-process), so the NAV exe was not built (dispatch).
Waited about 20 minutes at the gate (the foreign `C:\atx\build-vol` build; HOST-5 came after). Slip **M3-S1**
(logged in `builds.txt`): root edited `build.sh` while it ran, so the wrapper's own epilogue failed (its "exit 127"
line) after `research-build.ps1` returned; the build is evidenced by its receipt (ExitCode 0, source `b182dfdd`, exe
SHA-256 = disk) and log; receipt, build and cache-stats logs were copied by hand; the wrapper was rewritten after the
build and not touched during a build again.

### Tests (merged head `b182dfdd`; ENV-1: 0 hits in every log)

| suite | result |
|---|---|
| gtest `NavSpec.*:NavRules.*:CostLawRegistry.*:TradeRateRegistry.*:TargetRegistry.*:NavFieldsSeal.*:NavBookWorkers.*:NavBookRule.*:NavAnnMeanGross.*:CalibrateGross.*:CovStore.*` (p9-2u) | **34 / 34**, `NavSpec.GridFileRefusesADuplicateKey` OK (`gt-2u-c3b-named.log`) |
| pytest, the lane's eight wave files (`test_wave_gross_match.py` 12, `test_research_wave.py` 19, `test_wave_driver.py` 28, `test_wave_review.py` 21, `test_wave_review_minor.py` 9, `test_wave_scoreboard.py` 3, `test_wave_speed.py` 5, `test_no_python_mirror.py` 7), seed 0 | **104 passed**, 0 failed, 0 skipped (`pyt-c3b-wave8-s0.log`) |
| pytest `test_wave_kinds.py` (111) + `test_wave_eval_verb.py` (7), seed 0 | **118 passed**, 0 failed (`pyt-c3b-kinds-evalverb-s0.log`): B2's eval-verb waves run on the `pm6-6-engine` fixture default; the eval pin is inherited |
| classes guard (SQL2-CLS check) | **5 passed** (no new literal) |
| whole `scripts/tests` seeds 0 / 1, stage chain, whole target-tests, Release, canaries | deferred to the gate (TEST-1) |

### Root items ("What root must check at merge" of `task-C3b-review.md`; `task-C3-report.md` "Part B")

| item | result |
|---|---|
| 1 ROOT-1a: the wave suites + `test_wave_gross_match.py`, `test_wave_kinds.py`; Debug target-tests, named 34 | **holds** (above) |
| 2 keep-both with D3 (8 fixture lines, 3 `test_wave_kinds.py` hunks) | **carried to M4** (D3's merge). The five stale docstrings (`test_wave_kinds.py` `:543`, `:563`, `:584`, `:734`, `:737` at C3b's head; comments only) are left for D3's merge |
| 3 with B2: `wave_fixture.py`, `test_no_python_mirror.py`, `test_wave_kinds.py` merge cleanly; `test_wave_eval_verb.py` on the merged tree | **holds** (keep-both PASS; 118 passed) |
| 4 PRE-40: the two templates carry no `gross_match`; root's P9-M / P9-H manifests must name `pm6-6-engine` | **carried** to the root that writes the P9 manifests (wave-3 re-pin); `manifest_note` in the two rule files not edited (D3W-STALE / PRE-40) |
| 5 item 6 (C3B-6) | **done in M2** (`6c201a1d`) |
| 6 C3B-S evidence tools | **listed, broken by design, not rewritten**: `m2e-logs/tools/c2_gm_pair.py:156-159` (`WR.matched_leverage`, `WR.gross_matches`: both deleted from `scripts/wave_rules.py`); `m2h-logs/m2h2-engine-o1.py:125` (variant `"py"`, the retired `pm6-6`: run only the engine variants); `m1-logs/tools/fakewave_f1.py` and `sql3-evidence/sql3-fakewave-digests.py` (the fake wave on the fixture default, now `pm6-6-engine`: compare only with a run on the same head, never with the stored digests). The identity of record for the gm pair is the engine route's |
| 7 SQL2-CLS | **holds**: no new literal; guard 5 passed |
| 8 G-P5 `sign_pm7_35` row (`test_no_python_mirror.py:64-65`) still names lane `"E2"` | **open by rule (C3B-S)**: lane SIGN relabels it in its deletion commit, or root relabels it "P10 (SIGN not merged)" before `P9_FREEZE = True` |
| flag-absent identity (report procedure: NAV argv run, pre-C3b vs merged exe) | deferred to the gate (TEST-1); by construction a grid / NavSpec file with no repeated key parses to the same document |

### For PRE (round 2b-ii)

Appendix IV item 5 (C3-6 byte neutrality of `--ann-mean-gross`) stands from root M2j (item 4 HOLDS there); C3b adds no
flag and no output key (`parse_unique_keys` only refuses a repeated key), so no fact changes; the NAV flag-absent run is
the gate's.

## D3W

### Merge

- `git merge --no-ff f0726acb` (review APPROVE of `1e6c4336`; MERGE-SHA) -> **`08f04e07`**, **0 conflicts**; tree
  `a9f66958` = the recompute. Merge base `fd084e78`.
- 31 code files (+2,849 / -124): the composition fit (`fit.hpp`, `fit.cpp`, `composition_cli.{hpp,cpp}`,
  `weights_schema.hpp`), the eval verb (`eval_report.{hpp,cpp}`, `eval_cli.{hpp,cpp}`), the IC runner
  (`strategy_ic_{admission,composition,detail,mom_volman,rules,runner}.*`), their gtests, the fitter and its modules
  (`composition_engine_fit.py`, `composition_mom_volman.py`, `composition_resid.py`, `fit_composition_weights.py`),
  their pytest files, `test_fit_walk_forward.py` (new), `scripts/tests/test_research_spec.py` (D2R-3 S2).
- Records: `handoff-D3W.md`, `task-D3W-review.md` and `d3w-logs/` arrived; `task-D3W-report.md` is NOT in
  `f0726acb` (pool-23 commits it at `dcddb01d`, D3W-S S-1, above the APPROVE SHA): it arrives with D3W-PB's merge
  (the copy read for this merge is pool-23's).
- **B2-EVAL-KEEP / B2-14** (`m3-logs/d3w-keep-both.txt`): the four files both sides touch auto-merged, no region
  edited, no side checkout; every hunk of both sides kept: `eval_report.cpp` (B2 8 hunks at merged `:171-175`,
  `:177-178`, `:187-202`, `:583`, `:590-591`, `:605`, `:765-767`, `:843`; D3W 7 hunks at `:8`, `:25`, `:618-632`,
  `:634-656`, `:668-670`, `:720`, `:747-752`), `eval_report.hpp` (2 + 2), `research_eval_tie_test.cpp` (B2 2, D3W 1),
  `scripts/tests/test_research_spec.py` (root 7, D3W 1: the S2 assertion at `:944-945`).

### Build (EXE-CUR)

| tag | preset | targets | exit | TUs / links | wall | warning lines |
|---|---|---|---|---|---|---|
| **p9-2v** | equity-dev | `atx-research-eval`, `atx-engine-research-eval-tests`, `atx-engine-research-composition-tests`, `atx-impl-strategy-ic-tests`, `atx-research-composition`, `atx-research-admission`, `atx-equity-strategy-ic` | 0 | 33 / 9 | 139 s | 0 |

Exes (source `08f04e07`; on-disk SHA-256 = receipt `mega-p9-2v-receipt.json`): `atx-research-eval`
`6962966c4b9d419996e24df55ac825e6f6ccd7672956986ad0a3d5b1b5b68778`; `atx-engine-research-eval-tests`
`dd8a3103828b4a671c048d569eba4d817d0e91acf49bd2fb61983e5777ddb7ca`; `atx-engine-research-composition-tests`
`4e4ea8850e4ea7ef0f3b2b2d0ee6a06c4e1f615efe70fec5fb77c38dd1f333f4`; `atx-impl-strategy-ic-tests`
`0a2d0d9962ce1ee386f9fb72be16df77048ebf297289c8f09c25f44e0b0e9685`; `atx-research-composition`
`5e81e9c2d965de28dac83f10c4906aeb48d39113000ad843540768ba787884b1`; `atx-research-admission`
`411af33a0def5a6c28983dbf1ffc2fbc324694e8ac2e6b24401b27b693c9fa26`; `atx-equity-strategy-ic`
`a22c935dadc90781196343a608694eacf1fb60fd2cfecd454a10eb957c7e00c2` (the IC pair is superseded by p9-2w at ABS).
Waited about 11 minutes behind a pool-30 single-TU check (HOST-4: it started first).

### Tests (merged head `08f04e07`; ENV-1: 0 hits)

| suite | result |
|---|---|
| eval tests, whole target (B2-EVAL-KEEP: B2's 26 + D3W's `EvalVerb.VolFlagAddsNetVolOnly`) | **27 / 27** (`gt-2v-eval.log`) |
| `EvalVerb.HandRowAtNTotMinusMLeavesOutTheCampaigns:EvalVerb.VolFlagAddsNetVolOnly` (PRE-15 and the flag-absent eval document) | **2 / 2** |
| composition `WalkForward.*:MomVolman.*:ResearchComposition.*` (= the whole target) | **17 / 17** |
| IC `WeightsV3.*:CompositionWeightRows.*:MomVolman.*:ThemeTsmomRunner.*:CharCovHedge.*` | **18 / 18** |
| pytest (cwd pool root) `test_composition_engine_fit.py` 24, `test_composition_mom_volman.py` 9, `test_composition_resid.py` 23, `test_fit_composition_weights.py` 100, seed 0 | **155 passed, 1 skipped** (the engine-fit real-exe case without exes) |
| pytest `test_fit_walk_forward.py` 23 + `test_composition_engine_fit.py` 24 with `ATX_RESEARCH_ADMISSION_EXE` / `ATX_RESEARCH_COMPOSITION_EXE` = p9-2v | **47 passed, 0 skipped** (`RealExecutables` and the engine-fit real-exe case ran) |
| pytest `scripts/tests/test_research_spec.py` (D2R-3 S2), seed 0 | **87 passed** |
| classes guard | **5 passed** (`atx.dsl-composition-weights/v3` was already registered) |
| whole targets, Release, `scripts/tests` seeds 0 / 1, stage chain | deferred to the gate (TEST-1); the lane's seed-1 run was void (ENV-1), so the gate's is the first valid one |

### Root items ("What root must check at merge" of `task-D3W-review.md`; report "Flag-absent identity")

| item | result |
|---|---|
| 1 ROOT-1a build + named filters + pytest files | **holds** (above) |
| 2 B2-EVAL-KEEP (eval gtests and the PRE-15 gtest after both are in; `VolFlagAddsNetVolOnly` green) | **holds** |
| 3 identity (`d3w-logs/tools/ident.py` on `git archive b182dfdd` and `git archive 08f04e07`, the same WORK dir, then `cmp.py`) | **holds**: **17 of 21 files byte-identical; the other 4 differ only at `provenance.engine_fit.producer_fingerprint` and / or `provenance.resid.producer_fingerprint`**, exactly the lane's and the reviewer's result (`d3w-identity-cmp.txt`). Fingerprints (`d3w-fingerprints.py`): only `composition_engine_fit` and `composition_resid` move; the fitter's `OUTPUT_PRODUCERS` `d28a3089` and every other entry unchanged (`d3w-fingerprints-cmp.txt`). No pinned SHA of such a weights file was edited |
| 4 new IC output files `<role>_mom_volman_sleeve_{raw,scaled}.csv` on every mom-volman-v1 run | **noted** for the gate and P9-M (a directory digest or file list of such a run differs; the reviewer found no strict file-set check) |
| 5 EXE-CUR and trees | **holds** for root (p9-2v receipt). Pool-23's two trees (teeth-MUTATED Debug, never evidence; Release `p9-d3w-h`) are **kept**: D3W-PB's fix and its reviewer's teeth build `p9-d3wpb-c` run in pool-23 (D3WPB-SEQ) |
| 6 v1 / v2-only readers refuse v3 (`strategy_marginal_ic.cpp:267`, `book_diagnostics.py:743`, `research_mine.py:68`) | **carried to PRE / P9-W** (the cell path must not run them on a v3 file) |
| 7 D3 / D3W meeting point (`wave_kinds.py:86-90`) | **carried to M4** (D3's merge is the second of the two) |
| 8 D3W-STALE | **holds**: `scripts/specs/p9/rules/mom-volman-v1.json` unchanged by the merge (diff empty); prereg 2b-ii states it |
| 9 S-6 into PRE (sleeve files TRAIN-only; walk-forward with mom-volman keeps the full-TRAIN scale schedule) | **carried to PRE** |

### For PRE (round 2b-ii)

- Appendix IV item 14 (B.3 P9-M sleeve line keep / cut): **KEPT**. ALCOMB-6 landed in root: the IC exe writes
  `<role>_mom_volman_sleeve_raw.csv` / `_scaled.csv` for a mom-volman-v1 run with a recorded block row, and the line
  reads `atx-research-eval --vol --daily raw=<role>_mom_volman_sleeve_raw.csv --daily scaled=<role>_mom_volman_sleeve_scaled.csv`
  (each cell's `net_vol.vol_annual`, `net_vol.sharpe_annual`); no re-pin for a cut. PRE-38 stands.
- B.6 P9-W (item 18, lane D3's): the fit mode and the per-date weights producer as `task-D3W-report.md` "For PRE"
  states them, now in root (`--engine-fit --walk-forward expanding --walk-forward-lag 2 --walk-forward-step 21`).
- D3W-STALE beside PRE-40's line; review items 6 and 9 above.

## ABS

### Merge and SQL2-CLS

- `git merge --no-ff 6a3f5043` (review APPROVE of `5dae4f69`; MERGE-SHA), after D3W as the review's item 1 orders (ABS
  contains `f0726acb` through `58617960`) -> **`898bf120`**, **0 conflicts**; tree `639a26a5` = the recompute. Merge
  base `fcb0200e` (ABS had merged root's code head last).
- 11 code files (+742 / -53): `atx-impl/src/strategy_ic_absent.{hpp,cpp}` (new), `strategy_ic_composition.cpp` (S-3),
  `strategy_ic_detail.hpp`, `strategy_ic_rules.cpp` (the capability), `strategy_ic_runner.{hpp,cpp}`,
  `atx-impl/CMakeLists.txt` (3-line tail append), `atx-impl/tests/fixtures/composition_rules_list.json` (one capability
  line, regenerated by the lane's exe; ABS-3), `strategy_ic_rules_test.cpp`, `strategy_ic_runner_test.cpp`.
  `eval_report.cpp` is not in the merge's diff (ABS's auto-merged copy equals root's after D3W). Records
  (`handoff-ABS.md`, `task-ABS-report.md`, `task-ABS-review.md`, `abs-logs/`) arrived with the merge.
- **SQL2-CLS `094bcd3b`** (ABS-S, OD3P S-6; one commit, `classes.json` only, LF kept):
  - `atx.od3-flagged-members/v1` (read by `--absent-members`; OD3P's hand-written drop list): **its own class
    `od3-drop-list`**, placed after `diagnostic-spec` and shaped like it (`spec_doc`, `spec_kind` `other`, label I4),
    glob `**/*.json` gated by the schema (the reach the lane's proposed `cycle-spec` row would have had). Root's choice
    under S-6: `cycle-spec` would run `spec_pins` / `spec_follow` on a list that is not a cycle spec (the review's
    mislabel note); the new row records the list as a pinned input document, `pinned_by` naming
    `--absent-members-sha256` (`summary.json` `absent_members.list_sha256`) and the carry record's
    `fit.carry.drop.sha256`.
  - `atx.carry-weights/v1` (OD3P's `carry.json`, published in `fit.output`): class `fit-output` `schemas`; its `writer`
    gains `atx-impl/tools/carry_weights.py` (arrives with OD3P's merge; a registered literal not yet in the code does not
    turn the guard red).
  - Classes guard before **1 failed / 4 passed** (only `atx.od3-flagged-members/v1`, in `strategy_ic_absent.hpp`,
    `strategy_ic_runner.{hpp,cpp}`); after **5 passed** (97 subtests). Store suite (`test_research_store_{classes,blind,
    identity,fixtures}.py`, `test_research_store.py`; store exe p9-2r `d8a500d8` = its receipt, current by source)
    **43 passed** under `PYTHONHASHSEED` 0 and 1. Fixture chain (`m3-logs/tools/fixture_chain.sh`): rel and abs
    `files_seen 28 verified 25 declared 0 skipped 4`, **`catalog_digest d32f7655...4e69` unchanged**, `verify --pins` 0,
    identity 12 checked / 0 mismatches; the store parsed the new class row (`catalog` exit 0).

### Build (EXE-CUR)

| tag | preset | targets | exit | TUs / links | wall | warning lines | exes (source `094bcd3b`; disk = receipt) |
|---|---|---|---|---|---|---|---|
| **p9-2w** | equity-dev | `atx-equity-strategy-ic,atx-impl-strategy-ic-tests` | 0 | 26 / 3 | 212 s | 0 | IC `8608b8249258da827166454bd3b95c5be61625fcd2c4300e590d94e15723f857`; IC tests `fa383e75c7fb4aca66fe53117e419c4fc018e94c4032378f26ce3aff0254af52` |

The CMake tail append (new TU) regenerated the build files inside the build; no separate configure was needed.

### Tests (head `094bcd3b`; ENV-1: 0 hits)

| suite | result |
|---|---|
| `AbsentMembers.*` (p9-2w) | **11 / 11** (`gt-2w-abs-absent.log`) |
| the named identity filter `abs-logs/gtest-p9-abs-b-identity.filter` (incl. `CompositionRules.*`, which reads the regenerated fixture) | **53 / 53** (`gt-2w-abs-identity.log`) |
| D3W's IC filter re-run after ABS (`WeightsV3.*:CompositionWeightRows.*:MomVolman.*:ThemeTsmomRunner.*:CharCovHedge.*`) | **18 / 18** (`gt-2w-d3w-ic.log`) |
| the fixture's Python readers (`test_composition_mom_volman.py`, `test_composition_theme_hedge.py`, `test_fit_composition_rule_plugins.py`, `scripts/tests/test_wave_kinds.py`), seed 0 | **138 passed** (the review put them at the gate; cheap and merge-affected, so run here) |
| classes guard / store suite / fixture chain | as SQL2-CLS above |
| whole IC target, Release, canary goldens at 1 / 4 workers, `scripts/tests`, stage chain | deferred to the gate (TEST-1) |

### Root items ("What root must check at merge" of `task-ABS-review.md`)

| item | result |
|---|---|
| 1 order: after D3W, `--no-ff`, no IC conflict | **holds** |
| 2 SQL2-CLS `atx.od3-flagged-members/v1`; guard re-run | **holds** (`094bcd3b`; class choice stated above); `atx.carry-weights/v1` added in the same commit (dispatch) |
| 3 ROOT-1a: Debug build of the two targets; `AbsentMembers.*` 11; identity filter 53 | **holds** |
| 4 `--list-rules --json` on root's exe | **holds** (`abs-list-rules.txt`, `abs_caps.py`): IC exe p9-2w `8608b824...` (source `094bcd3b`), exit 0; `capabilities` sorted, `absent-members-v1` first (9 capabilities); stdout equals the committed fixture byte for byte (LF-normalised) and as JSON |
| 5 flag-absent identity (IC passes without `--absent-members`: byte-identical outputs modulo the timing keys) | deferred to the gate (TEST-1): the tiny-world canary's u / w passes against the goldens and the parents' identity runs; by construction (no new key, recipe term or cache key without the flag); the identity gtests above hold |
| 6 after OD3P's merge: `exe_capabilities` on the built exe | **holds now**: `scripts/cycle/phases.exe_capabilities` on p9-2w returns a set holding `absent-members-v1` |
| 6 after OD3P's merge: `test_research_cycle_od3.py`; the plan on `scripts/specs/p9/od3/` refusing with a fake exe lacking the capability (exit 2, naming `absent-members-v1`, the history roles, OD3P-1, "not read: no producer") and planning with it (history roles' u / w passes end in `--absent-members <path> --absent-members-sha256 <pin>`; the TRAIN anchor gets nothing) | **pending M4**: the od3 specs, the plan code and that test are OD3P's and not in root (OD3P merges in M4) |
| 7 EXE-FREEZE | **holds so far**: ABS is in root before the wave-3 P9-B0 re-pin; any later relink of the IC exe redoes P9-B0 and the re-pin first |
| 8 for the PM (ABS-3 (a) wording) | ruled (ABS-S); nothing for root |

### For PRE (round 2b-ii)

None of Appendix IV's items names ABS. F.1 / F.2 (OD3P-1, ABS-1): ABS is merged, so the clean-source line has a
producer (capability `absent-members-v1` on root's IC exe p9-2w); "not read: no producer" applies only if the gate fails.

### Disk and status (M3)

- Root's object trees `build-equity/` and `build-equity-rel/` kept (C: 45 GB free; the next root group reuses them).
  Lane pools: the leftover build trees of pool-15 (C3b) and pool-28 (ABS) may now be deleted by name (DISK-2, receipts
  and logs kept); not done by this group (not in its dispatch). Pool-23's trees are kept (D3W-PB, D3WPB-SEQ).
- Session scratchpad `root-m3/` (merge messages, `--basetemp` dirs, the D3W identity archives and outputs, the fixture
  chain catalogs): mine, regenerable; deleted by literal name at the end of the group.
- Code head **`094bcd3b`**. Build tags used p9-2u, p9-2v, p9-2w; **next free build tag p9-2x**. Known-red set: exactly
  `atx-impl/tools/test_nav_summ_v8.py::test_legacy_n37_numbers_reproduced` (root-only; B2-N37, merge `5dfb0ba1`); no
  new red. Trial ledger 133 lines `27e40f9f` at start and end (0 trials). SQL3-2 holds (`build-equity/research-store/`
  absent). No lease touched. No permission refusal. One slip (M3-S1, wrapper only).
