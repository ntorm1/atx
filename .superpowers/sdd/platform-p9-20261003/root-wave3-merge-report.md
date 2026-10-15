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

## Step group M4 (RESUME 10): D3, OD3P, AL-CLOCK, COV-MV

Root M4, 2026-10-09 UTC (ledger "Dispatch root M4"; `r10-dispatch.md` "ROOT M4"; the PM's prompt fixes the order D3,
OD3P, AL-CLOCK, COV-MV). Evidence: `m4-logs/` (tools `m4-logs/tools/`: `build.sh`, `busy.ps1`, `gt.sh`, `pyt.sh`,
`keep_both.py`, `warn_count.py`, `abs_caps.py` copied from `m3-logs/tools` with the logs dir changed; new:
`od3_plan_probe.py` (ABS item 6 / OD3P item 6), `m4_c3_probe.py` (C3-5 / C3-13, a copy of
`m2j-logs/tools/m2j_c3_probe.py` with the changed lines marked M4), `nav_rules_cmp.py` (two `nav --list-rules --json`
envelopes by row id)). Exit tables `m4-logs/gt-exits.txt`, `pyt-exits.txt`, `builds.txt`. Handoff:
`handoff-root-m4.md`. TEST-1 / ROOT-1a throughout: targeted checks at each merge; whole suites, Release, canaries,
golden and the parents' flag-absent identity runs are the gate's.

Start state (`m4-logs/start-state.txt`): docs head `f58e3a4c`, code head `094bcd3b`; tracked change only the PM's
`progress.md`; the five `docs/plans/*.png` untracked (not root's); trial ledger 133 lines `27e40f9f`;
`build-equity/research-store/` absent (SQL3-2). No housekeeping commit was needed (no merge touches `progress.md`).

MERGE-SHA at each slot: D3 `e197b3b6` and OD3P `dc11c977` as dispatched. AL-CLOCK: at its slot the ledger's newest
`Task AL-CLOCK: APPROVE` line is still `7ebd41ff` (first review `e9b3a323`); no fix-1 APPROVE line exists, so
`e9b3a323` was merged and fix round 1 (ALCLOCK-S, pool-30, in progress) is M5's second AL-CLOCK merge. COV-MV: at its
slot the ledger's newest line is `Task COV-MV: APPROVE at 25e11b32` (re-review 1 of fix round 1, code `942f499b`;
the PM's message said the same), so `25e11b32` was merged, not `e9e794b3`: no second COV-MV merge is owed.

## D3

### Merge

- `git merge --no-ff e197b3b6` (review APPROVE of `699440bb`; MERGE-SHA) -> **`0a2ceb2b`**, **0 conflicts**; tree
  `2ce6ea7a` = the `git merge-tree --write-tree` recompute. Merge base `685e238b`.
- 5 code files (+896 / -36): `scripts/wave_kinds.py`, `scripts/wave_stage_kinds.py`, `scripts/tests/test_wave_kinds.py`,
  `scripts/specs/p9/kinds/new-cost-model.json`, `scripts/specs/p9/kinds/walk-forward.json` (D3-2's four kind files).
  Records (`handoff-D3.md`, `task-D3-report.md`, `task-D3-review.md`, `d3-logs/`, `d3-configure.log`) arrived with it.
- **Keep-both** (`m4-logs/d3-keep-both.txt`, `keep_both.py`; every hunk of both sides survives, PASS):
  `new-cost-model.json` (C3b line 18; D3 line 5), `walk-forward.json` (C3b `:18`; D3 `:5`, `:29`, `:31-32`),
  `test_wave_kinds.py` (root 4 hunks: B2's `F.pin_eval_verb(root)` in `world()` at merged **`:187`** (root's `:171`),
  C3b's `:630-631` (`record_spec`), `:750` (`parametrize("mode", ["pm6-6-engine"])`), `:776` (the `pm6-6` branch
  removed); D3 8 hunks). C3b's eight fixture lines read `"gross_match": "pm6-6-engine"` at line 18 of
  new-combination-rule, new-cost-model, new-field, new-horizon, new-signal, new-universe, replaced-signal and
  walk-forward; leverage-change and new-nav-rule stay `"none"` (`d3-c3b-b2-lines.txt`). Nothing had to be re-added.
- No build (Python only).

### Tests (merged head `0a2ceb2b`, seed 0, `ATX_EQUITY_BIN` unset; ENV-1: 0 hits)

| suite | result |
|---|---|
| `scripts/tests/test_wave_kinds.py` | **191 passed / 1 skipped** (the skip: the live-table test, `ATX_EQUITY_BIN` unset). The dispatch expected 192 / 1: that is D3's own head; the D3 reviewer's three-way preview with C3b recorded **191 / 1** ("one fewer: C3b drops the `pm6-6` param of the match-stage test, C2-11"), which is what the merged tree gives |
| the C3-13 tests (`-k c3_13`) | **10 passed** |
| W3-8a: `test_the_ten_fixtures_load_and_cover_the_kinds` ("10 runnable, 0 schema only", `:216-217`), `test_the_cost_model_fixture_runs_to_spec_on_c3s_rows`, `test_every_fixture_runs_preflight_on_c3s_rows` (x 10) | **12 passed** |
| `scripts/tests/test_wave_eval_verb.py` | **7 passed** |
| classes guard (SQL2-CLS) | **5 passed** (D3 adds no literal; the two it names were registered before) |
| live-table test on real exes (`-k live_rule_tables`) | Debug: **1 passed** on p9-2x (after AL-CLOCK's row) and **1 passed** on p9-2y (after COV-MV's row); exes by receipt. Release: deferred to the gate (TEST-1; the Release exes are stale by source) |

### Root items (`task-D3-review.md` section 5; `handoff-D3.md` "Resume 9 close")

| item | result |
|---|---|
| 1 C3-13 / C3-5 probes (a) and (b) on real exes | **hold** (`m4-logs/c3-5-probe.txt`). Run at AL-CLOCK's head with the p9-2x NAV / IC exes: at D3's merge the Debug NAV exe of record (p9-2p) was stale by source after C3b / D3W / ABS (EXE-CUR, M3-Q2) and D3 is Python only. (a) parent `aim-partial-v5` at .05, wave names `aim-partial-v5 {trade_fraction .1}`: the cell, `-cal` and `-gm` specs set `nav.rule aim-partial-v5`; every planned and run NAV argv carries ONE `--rule aim-partial-v5` and one `--trade-fraction 0.1`; the calibrated cell's NAV run completed exit 0; verify done (wave exit 0). (b) parent `aim-partial-v6` (another aim rule; the live table has one `--rule` row), the same wave: the cell's `nav.rule` is set to `aim-partial-v5` (the parent's replaced), ONE `--rule aim-partial-v5` on every NAV argv of the cell, the NAV exe accepts, verify done. Two earlier attempts per shape stopped at verify on the probe world's setup (no eval verb pinned, B2-10 (c); then no NAV pin on the parent, W1-I2), fixed by `lock --exes` on the parent; the cell and argv tokens were the same in every attempt |
| 2 keep-both with C3b / B2 | **holds** (above) |
| 3 W3-8a G-P2 tick | **holds** (12 passed) |
| 4 `test_wave_kinds.py`, `test_wave_eval_verb.py` on the merged tree | **holds** (191 / 1 skipped; 7) |
| 5 live-table test on both builds | **Debug holds** (p9-2x, p9-2y); Release deferred to the gate (TEST-1) |
| 6 (and D3W review item 7, D3 S-3) the kind's walk-forward constants vs the fitter | **holds** (`d3-wf-flags.txt`): the four flag names, `expanding`, `--engine-fit`, the refused set (`--theme-tsmom`, `--theme-resid`, `--era` = the fitter's `pooled`), the bounds (lag >= 2, step >= 1, min_decisions >= 2) and the default 21 (D3-Q3) match `composition_engine_fit.py` / `fit_composition_weights.py:1923` |
| 7 gate items | deferred (TEST-1): whole `scripts/tests` seeds 0 / 1, the stage chain, the D3-Q2 `fields.output` grep and the D3-S1 `nav.rule` grep |
| C3b's five stale docstrings (`test_wave_kinds.py`) | **not edited**: comments only, in a W3-8a kind file (lane D3's); listed for P10 |

### For PRE (round 2b-ii, Appendix IV item 18: B.6 P9-W)

As `task-D3-report.md` "For PRE" states, now in root: manifest `atx.research-wave/v2`, kind `walk-forward`, no
`rule_cell` / `rules` / `candidates`, `gross_match` `pm6-6-engine`, `walk_forward {window expanding, lag 2, step 21,
min_decisions 21}` (all four named, D3-1; 21 explicit, D3-Q3). Fit mode: the parent must fit under `--engine-fit`
without `--theme-tsmom` / `--theme-resid` / `--era` (preflight refuses otherwise; D3-Q1's identity at the gate's
P9-B0 decides whether P9-F0h is such a parent). Per-date weights producer: the C++ composition fit through the fitter
post-step (D3W, in root since `08f04e07`), `atx.dsl-composition-weights/v3`. Template: generated at
`<parent dir>/<parent stem>-wf2s21m21-<wave>.json` (no SHA-256 before the parent is fixed); the generator blobs in root:
`scripts/wave_kinds.py` `d282586a`, `scripts/wave_stage_kinds.py` `694b619e` (= the reviewed blobs); the kind fixture
`scripts/specs/p9/kinds/walk-forward.json` sha256 `d865201a1f7a909c62f471f1c9ad58da87228c3c23e5fd28e45df0921b098d9f`.

## OD3P

### Merge

- `git merge --no-ff dc11c977` (re-review 1 APPROVE of `97a8157f`; MERGE-SHA) -> **`7f6f1c75`**, **0 conflicts**;
  tree `36c83320` = the recompute. Merge base `fd084e78`.
- 6 code files (+1,633 / -6): `atx-impl/tools/carry_weights.py`, `atx-impl/tools/test_od3_tooling.py` (new),
  `scripts/cycle/cli.py` (the 3-line T1 refusal), `scripts/research_roles.py`, `scripts/specs/p9/od3/flagged_members.json`
  (new; sha256 `9aa6fb59...f5ff`, = the review's), `scripts/tests/test_research_cycle_od3.py` (new). Records
  (`handoff-OD3P.md`, `od3p-absent-contract.md`, `od3p-evidence/`, `p9-od3-runbook.md`, `task-OD3P-report.md`,
  `task-OD3P-review.md`, `task-OD3P-rereview-1.md`, `task-XPRE-report.md`, `v8-od3-protocol.md`) arrived with it.
- Keep-both: `scripts/cycle/cli.py` (root 1 hunk at merged `:54-59`, OD3P 1 hunk at `:490-492`): PASS
  (`od3p-keep-both.txt`). Root's `research_roles.py` blob before the merge was `313b23b3` (the identity tests' pin; no
  lane changed it after the lane branched), after it `c293f40a` (OD3P's).
- No build (Python only).

### Tests and root items (`task-OD3P-rereview-1.md` "What root must check at merge"; ABS review item 6)

| item | result |
|---|---|
| 1 SQL2-CLS `atx.carry-weights/v1`, `atx.od3-flagged-members/v1` | **holds**: both in root since `094bcd3b` (no new row); classes guard **5 passed**. The class of `atx.od3-flagged-members/v1` is `od3-drop-list` (`spec_doc`, `spec_kind` `other`), root M3's choice that M3-Q1 accepted; `atx.carry-weights/v1` sits in `fit-output` with `carry_weights.py` in its `writer` |
| 2 the two lane files (`test_research_cycle_od3.py`, `atx-impl/tools/test_od3_tooling.py`) | **74 passed** (seed 0). Whole `scripts/tests` seeds 0 / 1 and the stage chain: the gate's (seed 1 has never run on the fix code) |
| 3 the `mv-aim-v1` entry of `flagged_members.json` (`rules_pending`) | **open for P0 (before the read), not M4's**: COV-MV is now merged and its registration names `source_sample_end` 2010, so under the review's rule (into `rules` if 2015 or later, else removed) the entry is removed and the file re-pinned at P0; until then line 4's plan is refused at plan time, as the runbook's P7 expects. ALCLOCK-2 is already applied (`info-clock-rate-v1` 2019 in `rules`) |
| 4 T2 (r7 item 6), OD3P-S S-a / S-d | the OD3P follow-up round (pool-21; M5 after its re-review 2) |
| 5 R-1 at the read | the read's (P7) |
| 6 after ABS: `--list-rules --json` lists `absent-members-v1`; the plan of a `fit.carry.drop` spec | **holds** (`od3p-plan-probe.txt`, `od3_plan_probe.py` on the lane test's fake world: history era E1, TRAIN anchor E2, the drop list): a fake IC exe lacking the capability -> `plan` exit **2**, before any step (no out dir), the message names `absent-members-v1`, "history roles E1", OD3P-1, "not read: no producer" and "nothing runs"; a fake offering it -> plan exit 0, E1's u and w passes end `--absent-members <list> --absent-members-sha256 <pin>` after `--candidate-cache` (the fake argv has no `--theme-registry`), E2's carry neither the flags nor the list; **root's real IC exe** (p9-2w `8608b824...`, source `094bcd3b`, current: no C++ / CMake change `094bcd3b..7f6f1c75`) behind the same `.cmd` shim -> the same plan, the same checks |
| 7 prereg F.1 names P9-F0 where PRE-39 names P9-F0h | round 2b-ii text (PRE) |

## AL-CLOCK

### Merge

- `git merge --no-ff e9b3a323` (review APPROVE of `7ebd41ff`; MERGE-SHA; branch `feat/p9-alclock2-20261007`, pool-30,
  never pool-8's) -> **`858a4fca`**, **0 conflicts**; tree `868ae10e` = the recompute. Merge base `685e238b`.
- 21 code files (+1,669 / -32): `atx-impl/src/book/nav_{cli,decide,load,lockstep,publish,rules,run,session,spec,
  validate,write}.cpp`, `nav_replay_internal.hpp`, `nav_rules.hpp`, `nav_spec.hpp`, `strategy_nav_replay.hpp`,
  `strategy_nav_v7.cpp` (ALCLOCK-3), `atx-impl/tests/CMakeLists.txt` (tail block), `strategy_nav_info_clock_test.cpp`
  (new), `scripts/specs/p9/rules/info-clock-rate-v1.json`, `scripts/specs/p9/templates/info-clock.json`,
  `scripts/tests/test_info_clock_rule.py`. Records (`handoff-AL-CLOCK.md`, `task-AL-CLOCK-report.md`,
  `task-AL-CLOCK-review.md`, `alclock-logs/`) arrived with it.
- Keep-both with C3b (`alclock-keep-both.txt`, PASS): `nav_run.cpp` (C3b 2 hunks / AL-CLOCK 8), `nav_spec.cpp` (C3b 4 /
  AL-CLOCK 1, the key at the table's end `:123-124`), `nav_spec.hpp` (2 / 1).
- **W3-9 tail appends hold:** the registry row `info-clock-rate-v1` is the last row of `trade_rates[]`
  (`nav_rules.cpp:75-82`, its `info_clock_refuses` `:57-58`); the parser entry `--rate info-clock-rate-v1`
  (`nav_cli.cpp:206`) and the switch `--info-clock-print` (`:51`); the NavSpec key `info_clock_print` at the end of the
  table (`nav_spec.cpp:124`). The NAV exe's table against the p9-2p table (`covmv-logs/d-nav-list-rules-p9-2p.json`):
  only `info-clock-rate-v1` added, no row changed, order kept (`nav-rules-cmp-2p-2x.txt`).

### Build (EXE-CUR)

| tag | preset | targets | exit | TUs / links | wall | warning lines | exes (source `858a4fca`; disk = receipt) |
|---|---|---|---|---|---|---|---|
| **p9-2x** | equity-dev | `atx-impl-strategy-target-tests,atx-equity-strategy-targets,atx-equity-strategy-ic` | 0 | 42 / 4 | 230 s (244 s with the wrapper) | 0 | target-tests `68a26abd619d0f15987788d84f4c40193ee2c2c244cea58effa69be3b018eba0`; NAV `d255a0c3b6914ddca792b5ab748fe796162472e191b769664b90a1f088b25b7f`; IC `84fdbae1e732fb1485cc88f6de38082a6380addb46ed71115104093e986bb44e` |

The NAV exe is in the build by M3-Q2 (stale by source after C3b / D3W / ABS; no `-Canary`, TEST-1). The IC exe was
added to the same build: it links `atx-impl-strategy`, which holds the changed `book/nav_*` sources, and the C3-5
probe's IC passes and the live-table test run it (EXE-CUR); its relink precedes the wave-3 P9-B0 (EXE-FREEZE holds).
`DirtyEntries 6` = `progress.md` and the five PNGs (no code). No host wait (no other build; 4.1 GB free).

### Tests (head `858a4fca`; ENV-1: 0 hits)

| suite | result |
|---|---|
| the lane's named filter `InfoClock*:NavSpec.*:NavRules.*:TradeRateRegistry.*:NavAnnMeanGross.*:CostLawRegistry.*:TargetRegistry.*` (p9-2x) | **25 / 25** = the lane's 24 + C3b's `NavSpec.GridFileRefusesADuplicateKey`; `InfoClock*` 13 (InfoClockRate 9 incl. `BookWorkersOneAndFourSameBytes`, `WorkspaceReserveOneRatePerName`; InfoClockPrint 4) (`gt-2x-alclock-named.log`) |
| C3b's named filter again (`nav_spec` / `nav_run` keep-both) | **34 / 34** (`gt-2x-c3b-named.log`) |
| `scripts/tests/test_info_clock_rule.py` (after D3's C3-13 merge, as the review asks) | **5 passed** |
| live-table test, Debug p9-2x | **1 passed** |
| classes guard | **5 passed** (run at `5c811b56`; AL-CLOCK adds no literal: its one `atx.` literal in added lines, `atx.nav-construction-grid/v1`, is registered in `nav-output`) |
| Debug whole target-tests, Release (named, whole), canaries, `scripts/tests` seeds 0 / 1, the flag-absent X-5 / Y-F0 identity and the opt-in check | deferred to the gate (TEST-1) |

### Root items (`task-AL-CLOCK-review.md` "What root must check at merge")

| item | result |
|---|---|
| 1 merge target (pool-30 branch, review commit), before COV-MV | **holds** |
| 2 C3-5 probe with `--rate info-clock-rate-v1` | **holds** (`c3-5-probe.txt`, wave `clk3`, `--until spec`): preflight .. spec done, wave exit 0; the cell sets no `nav.rule` (append route); its planned NAV argv carries `--rule aim-partial-v5` ONCE (from `nav.rule`) and `--rate info-clock-rate-v1` ONCE, `--trade-fraction .05` once (the parent's); no NAV run of the cell (the tiny world has no `ea_days_since`) |
| 3 W3-9 keep-both with COV-MV | at COV-MV's merge (below): **holds** |
| 3 do `mv-aim-v1` and `info-clock-rate-v1` compose? | neither row lists the other in `incompatible[]` (merged table); no P9 cell names both; **open for the PM** (the review asked root to confirm with the PM) |
| 4 SQL2-CLS | **holds** (none) |
| 5 1-vs-4 `--book-workers` bytes; `test_info_clock_rule.py` | **holds** (`InfoClockRate.BookWorkersOneAndFourSameBytes` in the 25; 5 passed) |
| 6 before the first P9 cell (W1-I2 re-pin, ALCLOCK-1 `--info-clock-print` on the Y-F0 reference, `ea_days_since` in Y-F0's fields manifest, ALCLOCK-4's prereg line) | the gate's / P9-B0's / PRE's. ALCLOCK-2 holds already in root (`flagged_members.json` `rules` holds `info-clock-rate-v1` 2019) |
| 7 gate items | deferred (TEST-1) |

### For PRE (round 2b-ii, Appendix IV item 16: B.4 P9-C)

As `task-AL-CLOCK-report.md` "For PRE: the P9-C block" states, now in root at `858a4fca`: rule and row
`info-clock-rate-v1` (kind `trade-rate`, x-flag `--rate info-clock-rate-v1`, no params, incompatible `per-name-v1,
two-speed-v1, aim-partial-v6, spo-v1, spo-v2, spo-v3`); registration `scripts/specs/p9/rules/info-clock-rate-v1.json`
sha256 `51b8b754b7e16f49d2185a5e27bf5fa1260b8890f34490cea82d0bc1ffa4e33c` (= the report's), `source_sample_end` 2019;
template `scripts/specs/p9/templates/info-clock.json` sha256
`95d4104a421c32f91877649f1ec9dfbdb409c9a121d6f6e2377e5e25e19b42d5` (= the report's). **These are the pre-fix values:**
fix round 1 (ALCLOCK-S S-2) widens the registration's realised-leverage sentence, so the registration SHA-256 moves
at M5's second AL-CLOCK merge and is recorded there. C3-5 probe: holds (above). Fit mode: the parent's (NAV-only
cell), at the wave-3 P9-B0. Producers, C3-4, DEC-16 argument: as the report.

## COV-MV

### Merge

- `git merge --no-ff 25e11b32` (re-review 1 APPROVE of fix round 1 `942f499b`; first review `e9e794b3` of `846739c1`;
  MERGE-SHA; branch `feat/p9-covmv2-20261007`, pool-31, never pool-24's) -> **`5c811b56`**, **1 conflict**, the
  expected one: the `atx-impl/tests/CMakeLists.txt` tail. Resolved by an edit of that region (B2-14; no side
  checkout): AL-CLOCK's block FIRST (its blank line, comment, `strategy_nav_info_clock_test.cpp`), then COV-MV's
  three lines unchanged (comment, `strategy_mv_aim_rule_test.cpp`). Tree `8568b855` differs from the recompute
  `5991248f` only in that file. Merge base `094bcd3b` (the lane merged root's code head in its fix round).
- `atx-impl/CMakeLists.txt` was resolved inside the lane (its merge `1df31bcc`: ABS's `strategy_ic_absent.cpp` row,
  then COV-MV's `strategy_mv_aim.cpp` row) and reaches root that way; `atx-engine/CMakeLists.txt` gains
  `src/book/mv_target.cpp` in the `atx-engine` library.
- 18 code files (+2,698 / -20) vs `858a4fca`: `atx-engine/{CMakeLists.txt, include/atx/engine/book/mv_target.hpp,
  src/book/mv_target.cpp, tests/book/book_mv_target_test.cpp}`, `atx-impl/CMakeLists.txt`,
  `atx-impl/src/book/nav_{decide,lockstep,rules}.cpp`, `nav_rules.hpp`, `atx-impl/src/strategy_mv_aim.{cpp,hpp}`,
  `strategy_nav_v7.{cpp,hpp}`, `atx-impl/tests/CMakeLists.txt`, `strategy_cov_model_test.cpp`,
  `strategy_mv_aim_rule_test.cpp`, `scripts/specs/p9/rules/mv-aim-v1.json`, `scripts/specs/p9/templates/mv-aim.json`.
  Records (`handoff-COV-MV.md`, `task-COV-MV-report.md`, `task-COV-MV-review.md`, `task-COV-MV-rereview-1.md`,
  `covmv-logs/`) arrived with it.
- Keep-both (`covmv-keep-both.txt`, PASS; base `094bcd3b`, ours `858a4fca`, theirs `25e11b32`): `nav_decide.cpp`
  (AL-CLOCK 5 hunks / COV-MV 1), `nav_lockstep.cpp` (5 / 1), `nav_rules.cpp` (2 / 3), `nav_rules.hpp` (1 / 1),
  `strategy_nav_v7.cpp` (1 / 16), `atx-impl/tests/CMakeLists.txt` (1 / 1, merged `:210-214` then `:215-217`).
- **`nav_workspace_reserve_bytes`** (`book/nav_lockstep.cpp`): the body has exactly two additions over `685e238b`,
  AL-CLOCK's `clock` term inside the names product and COV-MV's trailing `+ v7::extension_workspace_bytes(names)`.

### Build (EXE-CUR)

| tag | preset | targets | exit | TUs / links | wall | warning lines | exes (source `5c811b56`; disk = receipt) |
|---|---|---|---|---|---|---|---|
| **p9-2y** | equity-dev | `atx-impl-strategy-target-tests,atx-engine-book-tests,atx-equity-strategy-targets,atx-equity-strategy-ic` | 0 | 36 / 6 | 165 s (174 s) | 0 | target-tests `ee69e2fb06db6702b78a9e8cbb68a9a13ca25ccc5fa578ada0c3d90b9afd8b8e`; book-tests `6977a5a75b386d283c81e148c0fec31a2da1f95536dc98cab2366d424d1f8360`; NAV `e6b21410987a8946dcd9a5d0b5631a15fee76d682d2be987c46f1d05be4bd219`; IC `fbb5b3928ebb994ed14d87efe8d28c9929af94636d666433151d33cc5e40a314` |

The NAV exe is rebuilt because `nav --list-rules --json` must come from the merged head (COV-MV changes its table);
the IC exe for the live-table re-run (it links `atx-impl-strategy`). This is the first clean build of a head holding
D3W, ABS, AL-CLOCK and COV-MV's fix round together. All exes re-hashed at the end of the group = their receipts
(SPDLOG-RELINK; `exe-sha-end.txt`).

### Tests (head `5c811b56`; ENV-1: 0 hits)

| suite | result |
|---|---|
| `MvTarget.*` (book-tests) | **12 / 12** incl. `FactorEigenOnKeptStorageIsAFreshSolve` |
| `MvAimRule.*` (target-tests) | **16 / 16** incl. `PrintFailureLeavesTheBookAndWritesNull`, `WorkspaceReserveTermOnlyWithTheRule`, `BookWorkersParity`, `ListRulesRowIsAClosedTargetRow` |
| the lane's named filter `Cov*:NavBookRule.*:VolTarget.*:Spo*:NavSpec.*:NavRules.*:CostLawRegistry.*:TradeRateRegistry.*:TargetRegistry.*:NavAnnMeanGross.*` | **100 / 100** (99 + C3b's `NavSpec.GridFileRefusesADuplicateKey`; the fix round's two new tests are in `MvTarget.*` / `MvAimRule.*`, outside this filter), 1 DISABLED (COVR's diagnostic, as before) |
| AL-CLOCK's named filter again (both reserve tests) | **25 / 25** |
| `nav --list-rules --json` (p9-2y) vs p9-2x | exit 0, stderr empty; 12 -> 13 rows: only `mv-aim-v1` added, no row changed, order kept; `mv-aim-v1` is the last row and the only `target` row, x-flag `--target-rule mv-aim-v1`, `incompatible` `[aim-partial-v6, spo-v1, spo-v2, spo-v3, two-speed-v1, adv-hold-v1, hold-band-v1]` (S-5: `two-speed-v1`) (`nav-rules-cmp-2x-2y.txt`); vs p9-2p: exactly `info-clock-rate-v1` and `mv-aim-v1` added |
| the lane's `covmv-logs/validate_specs.py` against the p9-2y table | exit 0: template and registration SHAs as pinned, no rule-cell or registration problem, `rule_flags` problems none (`covmv-validate-specs-2y.log`) |
| live-table test, Debug p9-2y | **1 passed** |
| classes guard (SQL2-CLS) | **5 passed**; COV-MV adds no literal (its one `atx.` literal in added lines is the registered `atx.nav-construction-grid/v1`) |
| Release, whole target-tests, canaries, gold-run 1 / 4 workers, the X-5 / Y-F0 rule-absent identity | deferred to the gate (TEST-1; COVMV-BYTES (c)) |

### Root items (`task-COV-MV-rereview-1.md` section 6; `task-COV-MV-review.md` section 7)

| item | result |
|---|---|
| 1 conflicts (W3-9): one, the tests CMake tail, AL-CLOCK first; `atx-impl/CMakeLists.txt` ABS then COV-MV | **holds** |
| 2 `nav_workspace_reserve_bytes` both terms; `MvAimRule.WorkspaceReserveTermOnlyWithTheRule` and `InfoClockRate.WorkspaceReserveOneRatePerName` | **holds** |
| 3 the rule table | **holds** |
| 4 the two new tests, `MvTarget.*` 12, `MvAimRule.*` 16, named 100, `InfoClock*` | **holds** |
| 5 flag-absent identity | by the round's construction (no path without `--target-rule mv-aim-v1` changed; the reserve value did not move); the X-5 / Y-F0 runs are the gate's |
| 6 SQL2-CLS | **holds** (none) |
| 7 pins | **hold**: store `ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3` in the registration (2) and the template (3); registration sha256 `4b4af6e238b529d5d7eabccd2699022dc653da52fa45801eb3f61896bd6680b4`, template `83391f2c526b28008d48e250f0e7d26e7c3f8661786d43a8f3d76439f664d870` (= the review's); S-b (the null form's sentence) is PRE's / the re-pin's |
| 8 the right branch | **holds** (pool-31's `feat/p9-covmv2-20261007` at `25e11b32`) |
| first review's item 5 (1 vs 4 workers with the rule on) | `MvAimRule.BookWorkersParity` passed; the gold-run check is the gate's |

### For PRE (round 2b-ii, Appendix IV item 17: B.5 P9-MV)

Merged and APPROVE (`25e11b32`, fix round 1 in). Registration `scripts/specs/p9/rules/mv-aim-v1.json` sha256
`4b4af6e238b529d5d7eabccd2699022dc653da52fa45801eb3f61896bd6680b4`, template `scripts/specs/p9/templates/mv-aim.json`
sha256 `83391f2c526b28008d48e250f0e7d26e7c3f8661786d43a8f3d76439f664d870` (root re-pins the template's
`template_sha256`, and so the registration's SHA, when it sets the template's `parent`: PRE-29); `source_sample_end`
2010 (reviewer-checked); the row as above; the store pin `ea58691f...f0f3` unchanged (PRE-9 / COV-13 (c) / PRE-24a
carried as the lane's registration states). Fit mode: at the wave-3 P9-B0. OD3P's P0 consequence: 2010 < 2015, so
`mv-aim-v1` leaves `flagged_members.json`'s `rules_pending` (re-pin at P0).

### Disk and status (M4)

- Root's object trees `build-equity/` and `build-equity-rel/` kept. The C3-5 probe worlds `build-equity/p9-m4-tw-*`
  (12 dirs, about 25 MB each, synthetic, regenerable) kept as the probe's evidence beside M2j's.
- Lane pools: nothing deleted (DISK-2 deletions are not in this dispatch; pool-30's fix round is running).
- Session scratchpad `root-m4/` (merge messages, `--basetemp` dirs, the od3 plan probe's fake worlds): mine,
  regenerable; deleted by literal name at the end of the group.
- Code head **`5c811b56`**. Build tags used p9-2x, p9-2y; **next free build tag p9-2z**. Known-red set: exactly
  `atx-impl/tools/test_nav_summ_v8.py::test_legacy_n37_numbers_reproduced` (root-only; B2-N37, merge `5dfb0ba1`); no
  new red in M4. Trial ledger 133 lines `27e40f9f` at start and end (0 trials; every probe wave stopped at verify or
  spec, with its own empty ledger). SQL3-2 holds. No lease touched. No permission refusal. One harmless duplicate in
  `pyt-exits.txt`: the line labelled `alclock-classes-guard-recheck` is a second classes-guard run on `5c811b56` (same
  result as `covmv-classes-guard`), not a run at AL-CLOCK's head.

## Step group M5 (RESUME 11): B2 fix 2, OD3P follow-up, D3W-PB, SIGN, A4

Root M5, 2026-10-15 UTC (ledger :835 "Dispatch root M5" and the RESUME 11 block; `r11-dispatch.md` "ROOT M5"; order
B2 fix 2, OD3P follow-up, D3W-PB, SIGN, A4). Evidence: `m5-logs/` (tools `m5-logs/tools/`: `build.sh`, `busy.ps1`,
`gt.sh`, `pyt.sh`, `keep_both.py`, `warn_count.py` copied from `m4-logs/tools`, `fixture_chain.sh` from `m3-logs/tools`,
with the logs dir changed; new: `mirror_freeze.py` (what the G-P5 freeze test would report with `P9_FREEZE = True`,
without editing the file), `resolve_concat.py` (B2-14 region edit: keep both, root's lines first)). Exit tables
`m5-logs/gt-exits.txt`, `pyt-exits.txt`, `builds.txt`; every merge tree against `git merge-tree --write-tree` in
`merge-trees.txt`; exe hashes at the end in `exe-sha-end.txt`. Handoff: `handoff-root-m5b.md` (`handoff-root-m5.md`
stays as the STOP 10 record). TEST-1 / ROOT-1a throughout: targeted checks at each merge; whole suites, Release,
canaries, golden and the parents' flag-absent identity runs are the gate's.

Start state (`m5-logs/start-state.txt`): docs head `e3b6c111`, code head `5c811b56`; tracked changes only the PM's
`progress.md` and `next-goal-prompt.md`; five untracked `docs/plans/*.png` (not root's, never added); trial ledger 133
lines `27e40f9f`; `build-equity/research-store/` absent (SQL3-2); no build of `C:\atx-wt\` running; free RAM about
1.5 GB of 16 GB, C: 57 GB free. **Step 0** `6cb1070f`: `progress.md` and `next-goal-prompt.md` as they stood, plus
`status-11.md`, `resume11-addendum.md`, `r11-dispatch.md` (`git add -f`); no ledger line edited, no PNG, no cache dir.
The PM appended three ledger lines during the group (AL-CLOCK fix-1 APPROVE `eed512bd`, ALCLOCK-S2, a PRE 2b-ii line:
all M6 / PRE matters); they are committed as they stand with the records commit.

MERGE-SHA at each slot: B2 fix 2 `101f695a`, OD3P follow-up `4a7372e2`, D3W-PB `c044f5f6` -> `460f5ba2` -> `44a331e6`,
SIGN `c2010ed1` -> `50a43599` -> `e2844926`, A4 `6dab6948` (then the flips and `fc3e414e` only after A4-Q2's TRAIN
identity). At A4's slot the ledger's newest `Task A4: APPROVE` line is still `fc3e414e` (no fix-1 line). Results: B2,
OD3P, D3W-PB and SIGN merged whole; **A4 stopped after its pre-flip head** (A4-Q2's TRAIN inputs live under
`C:/atx/atx-db/data/`, which the owner rule forbids touching: see "A4").

Builds of the group (all Debug `equity-dev` through `scripts/research-build.ps1`, target-scoped, exit 0, 0 warning lines,
no other build of `C:\atx-wt\` at any admission (HOST-4 / HOST-5); code dirty 0 at each, DirtyEntries = the five PNGs
plus, for p9-3b / p9-3c, the PM's uncommitted ledger lines):

| tag | source | targets | TUs / links | wall | exes (disk = receipt at every run and at the end) |
|---|---|---|---|---|---|
| **p9-2z** | `138e1d2c` | `atx-research-eval` | 0 / 1 (relink: `atx::engine` gained COV-MV's `mv_target.cpp` in M4, so p9-2v's verb was stale by source) | 16 s | eval verb `4791019131c0d74603b83cb772ef027ec8641167fd95844c4674e75bdf39893b` |
| **p9-3a** | `6088bfe9` | `atx-engine-research-composition-tests,atx-impl-strategy-ic-tests,atx-research-composition,atx-research-admission` | 9 / 5 | 42 s | composition-tests `76b664b7483a77a717015a3fdebb38f97dc39fbcfecd04a98e52d3cd016023a9`; IC tests `b43d7a3e...` (overwritten by p9-3b); composition `212f8b9941f0be705ebac138c03e83279fc863ab12051f00dd09501db7eed98e`; admission `411af33a0def5a6c28983dbf1ffc2fbc324694e8ac2e6b24401b27b693c9fa26` (bytes = p9-2v's) |
| **p9-3b** | `1dd9b9b1` | `atx-equity-strategy-ic,atx-impl-strategy-ic-tests` | 4 / 3 | 30 s | IC exe `ef3974cfa82d1cc86d6a8079394e5d0964e8abcea8fd36a068b12f8b8bc99b38`; IC tests `9cd667c75c08a9197f4267c7dc1e407630beb84f9163739227e73f8d28eedf56` |
| **p9-3c** | `bba4c06f` | `atx-engine-research-fields-tests,atx-research-fields` | 33 / 3 | 90 s | fields-tests `39be2b3d995b11873b5d667c04419b5b063f1f2f56729bce1816eed1b30febdf`; fields exe `d14346e836923cc8dea8ebd05b9769f0449b44e0f9f9150866f31d5a2e875cde` |

Tag naming: after `p9-2z` the series continues `p9-3a`, as `p9-1z` -> `p9-2a` did (no `mega-p9-3*` file existed).

## B2 fix 2

### Merge

- `git merge --no-ff 101f695a` (re-review 3 APPROVE of `509f4b79`; fix `71abeb2f`; MERGE-SHA) -> **`6d47b28d`**,
  **0 conflicts**, tree `6dbe1225` = the recompute. Merge base `858a4fca`. One code file, `atx-impl/tools/test_nav_summ_v8.py`
  (+73 / -5); records arrived with it (`task-B2-rereview-3.md`, the report's "Fix round 2", `handoff-B2.md`,
  `b2-evidence/r10-*`).
- No golden moved: `build-equity/mega-nav-v71-summ-n37.json` sha256 `98b44b24...9f98`, `mega-nav-v71-pbo-n37.json`
  `bc112b38...b1ca` (= the pins in `docs/plans/mega-alpha-v7-pitch.config.json`), mtime 2026-09-29, before and after
  (`m5-logs/b2-golden-sha-before.txt`).
- **B2-S3 S-1 slip** `138e1d2c` (code commit, text only): the test's docstring sentence now reads "A failure names JSON
  paths only when run with the blind flags (`--tb=no --show-capture=no -rN`), which every real-data run uses." No
  assertion, tolerance, golden or hash changed.

### Tests and root items (`task-B2-rereview-3.md` "What root must check at merge")

| item | result |
|---|---|
| 1 merge plain | **holds** (tree = recompute; the lane's only code file has no other writer in range) |
| 2 no golden moved | **holds** (above) |
| 3 the n37 test on root's current Debug verb | EXE-CUR: p9-2v's verb was stale by source (`handoff-root-m4.md` "Exes"), so **p9-2z** relinked it at `138e1d2c` (the merge + slip; no eval C++ / CMake differs to the head of the group). `ATX_EQUITY_ROOT=C:/atx-wt/pool-2/build-equity`, `ATX_EQUITY_BIN=C:/atx-wt/pool-2/build-equity/bin`, `pytest atx-impl/tools/test_nav_summ_v8.py --tb=no --show-capture=no -rN -p no:cacheprovider`: **10 passed, 0 skipped**, `test_legacy_n37_numbers_reproduced` among them (first run of the fix on a verb with D3W's eval hunks). **The known-red set is EMPTY.** |
| 4 at the gate (Release verb, 1e-12) | carried to the gate |
| 5 nothing else | **holds** (no C++, flag, key, rule id or literal; SQL2-CLS none) |

## OD3P follow-up

### Merge

- `git merge --no-ff 4a7372e2` (re-review 2 APPROVE of `d99a12bb`; code `cc6686f9`; MERGE-SHA) -> **`a2cf7e78`**,
  **0 conflicts**, tree `dc9491c1` = the recompute. Merge base `858a4fca`. Five code files (+509 / -14):
  `atx-impl/tools/{carry_weights.py, nav_summ.py, test_od3_tooling.py}`, `scripts/research_roles.py`,
  `scripts/tests/test_research_cycle_od3.py`. No build (Python); the verb is p9-2z (current).
- **OD3P-S2 S-4 slip** `b991c755` (code commit, `classes.json` only): the `od3-drop-list` row's `pinned_by` now reads
  "... the carry record's drop.list_sha256 (the cycle spec's fit.carry.drop.sha256)". Conditions of the ruling hold:
  classes guard 5 passed; store suite (`test_research_store_{classes,blind,identity,fixtures}.py`,
  `test_research_store.py`; store exe p9-2r `d8a500d8` = receipt, current by source) **43 passed** at seeds 0 and 1;
  fixture chain (rel and abs) `files_seen 28 verified 25 declared 0 skipped 4`, **`catalog_digest d32f7655...4e69`
  unchanged**, `verify --pins` 0, identity 12 / 0; no fixture digest or pin names the file's bytes (git grep of its blob
  and SHA-256: none).

### Tests and root items (`task-OD3P-rereview-2.md` "What root must check at merge")

| item | result |
|---|---|
| 1 the two lane files with the verb built (`scripts/tests/test_research_cycle_od3.py`, `atx-impl/tools/test_od3_tooling.py`, seed 0, verb = `NS.default_engine_exe()` = pool-2's p9-2z) | **96 passed, 0 failed, 0 skipped** (the OD3P-SKIP `real` cases ran; `test_carry_replaces_the_pooled_fit_and_feeds_every_era` passed: OD3P-ENV closed; `test_flag_absent_nav_summ_equals_roots_blob` ran) |
| 2 flag-absent identity on root's blob | **holds**: root's `nav_summ.py` was blob `9fd6bd0a` at the merge's first parent; the identity test's `[real]` param passed |
| 3 `--pool-one-sided` | **holds**: no tracked spec, template, kind or cycle code passes it (git grep over `scripts/`, `atx-impl/strategies`, `atx-engine`: none); outside `nav_summ.py` / its test only the runbook names it (`p9-od3-runbook.md:80` tool list, `:207` R4) (`m5-logs/od3p-pool-one-sided.txt`) |
| 4 SQL2-CLS | **holds** (no new literal; guard 5) |
| 5 merge preview clean | **holds** |
| 6 root's carried items | the `mv-aim-v1` re-pin of `flagged_members.json` stays P0's (MV-FLAG-P0); PRE-39 F.1 wording and seeds 0 / 1 + the stage chain are the gate's |

Note for the OD-3 root group: `p9-od3-runbook.md:207` still carries "[UNKNOWN: root's merge of the OD3P follow-up]"; that
merge is now `a2cf7e78` (not edited here: the runbook is the OD-3 group's input, filled at P0).

## D3W-PB

### Merges (MERGE-SHA chain `c044f5f6` -> `460f5ba2` -> `44a331e6`; D3W `f0726acb` already in root since M3)

- `git merge --no-ff c044f5f6` (pre-deletion head) -> **`6088bfe9`**, **0 conflicts**, tree `0303fd7f` = the recompute.
  Merge base `f0726acb`. 15 code files (+507 / -133): composition `fit.{hpp,cpp}`, `composition_cli.{hpp,cpp}`,
  `ew_theme_std.hpp`, `mom_volman.hpp` (new header), `research_composition_fit_test.cpp`, `strategy_ic_mom_volman.{hpp,cpp}`,
  `strategy_ic_rules.cpp`, `strategy_ic_mom_volman_test.cpp`, `composition_engine_fit.py` and three fitter tests.
  `strategy_ic_rules.cpp` auto-merged with root's side (B2): keep-both PASS (root 1 hunk, lane 17 hunks; `d3wpb-keep-both.txt`).
- `git merge --no-ff 460f5ba2` (the D2-3 deletion) -> **`175edd61`**, 0 conflicts, tree `02511ed6` = the recompute;
  `composition_resid.py` (-136), `test_composition_resid.py`, `test_no_python_mirror.py` (auto-merge keep-both PASS:
  root 2 hunks, lane 1).
- `git merge --no-ff 44a331e6` (the resumed review's APPROVE; fix `90bc1962`) -> **`22caee08`**, **1 conflict, S-g**, in
  `scripts/tests/test_no_python_mirror.py` comment lines, resolved by a region edit (B2-14): root's B2 line ("# P9 B2
  retired the statistics-of-record rows ..."), then the fix's two D3WPB-Q1 lines; the old two-line D2-3 comment dropped;
  root's deletion of the B2 rows kept; the four `retired_by` labels `P10` from the fix. The tree differs from the
  recompute only in that file.

### Identity (the report's TRAIN identity procedure; `d3w-logs/tools/pb-ident.py`, `cmp.py`, `d3w-fingerprints.py`)

| step | result |
|---|---|
| 1 fingerprints at `6088bfe9` (cwd `atx-impl/tools`) | **= the lane's `pb-fingerprints-pre-deletion.json` byte for byte**; `OUTPUT_PRODUCERS (fit, fit_prior)` `d28a30895ad1393e512d3a20384af025063675e9a2617611ee9cae41e17304a2` (`d3wpb-fingerprints-c044.json`) |
| 2-3 fitter outputs, BASE = `git archive f0726acb` vs the merged tree, the SAME `WORK` dir | every case exit 0; **21 of 24 files byte-identical; the 3 `engine-*` `composition_weights.json` differ only at `$.provenance.engine_fit.producer_fingerprint`** (`d3wpb-identity-c044.log`). The procedure runs the fitter with the test module's Python stand-ins, so no exe enters it |
| after `460f5ba2` | fingerprints unchanged (`d3wpb-fingerprints-460f.json`); identity again 21 / 24 with the same 3 differences (`d3wpb-identity-460f.log`) |

### Tests (ENV-1: 0 hits)

| suite | result |
|---|---|
| composition filter `AimV2.*:WalkForward.*:MomVolman.*:ResearchComposition.*` (p9-3a) | **21 / 21** |
| IC filter `WeightsV3.*:CompositionWeightRows.*:MomVolman.*:ThemeTsmomRunner.*:CharCovHedge.*` (p9-3a; again on p9-3b) | **18 / 18** (both) |
| the five fitter pytest files (`test_composition_engine_fit`, `_mom_volman`, `_resid`, `test_fit_composition_weights`, `test_fit_walk_forward`) with `ATX_RESEARCH_ADMISSION_EXE` / `ATX_RESEARCH_COMPOSITION_EXE` = root's p9-3a exes (never pool-23's mutated bin) | **180 passed, 88 subtests, 0 skipped** |
| after `460f5ba2`: `test_composition_resid.py`; `test_no_python_mirror.py` (own invocation) | **23 passed** (18 subtests); **7 passed**, G-P5 12 functions in 5 rows |
| after `44a331e6` (resolved): `test_no_python_mirror.py` (own invocation) | **7 passed**, "G-P5: 1 mirrored rule function(s) in 1 row(s)"; with `P9_FREEZE` on (`mirror_freeze.py`) the only row left is E2's `scripts/wave_rules.py` `sign_pm7_35` (SIGN's) |

### Root items (`task-D3W-PB-review.md` "What root must check at merge")

Order (1) **holds**; targeted checks (2) **hold**; SQL2-CLS (3) none (the added lines use the registered
`atx.composition-fit-spec/v1` and `atx.dsl-composition-weights/v3`); mirror guard at the freeze (4) **holds** (no
composition row blocks it); EXE-FREEZE (5): the IC exe and `atx-research-composition` relinked here, before the gate's
P9-B0 re-pin; stretch (6): APPROVE and merged; disk (7): pool-23's `build-equity/` (mutated `p9-d3wpb-c` binaries) not
touched and never used (TEETH-BIN-23).

### For PRE (round 2b-ii)

Nothing new: no P9 cell reads ew-theme-aim-v2's C++ fit (fit mode of every cell = its P9-B0 parent's, ALCOMB-7);
D3WPB-Q2 as the PRE list records it (P9-W unaffected). The D3W-PB report's open question (walk-forward on the aim
rules) stays the PM's / P10.

## SIGN

### Merges (MERGE-SHA chain `c2010ed1` -> `50a43599` -> `e2844926`)

- `git merge --no-ff c2010ed1` (pre-deletion head; code `8727f6d4`) -> **`ad913bd5`**, **2 conflicts, the expected
  CMake tails**, resolved keep-both by a region edit (`resolve_concat.py`, root's lines first): `atx-impl/CMakeLists.txt`
  (ABS's row and COV-MV's row, then SIGN's `strategy_sign_verb.cpp` source and `target_link_libraries(atx-impl-strategy
  PUBLIC atx::engine-research-admission)`) and `atx-impl/tests/CMakeLists.txt` (AL-CLOCK's and COV-MV's blocks, then
  SIGN's `strategy_sign_verb_test.cpp` in `atx-impl-strategy-ic-tests`). The tree differs from the recompute only in
  those two files. 12 code files (+1,243 / -9). Keep-both (`sign-keep-both.txt`): `atx-impl/CMakeLists.txt` PASS,
  `test_no_python_mirror.py` (auto-merged) PASS; `atx-impl/tests/CMakeLists.txt` reports FAIL in the strict form only
  because both sides open with the same added blank line, which git keeps once: the blank-insensitive check (same file)
  shows every non-blank line of both sides in the merge, root's 7 then SIGN's 3, in order.
- **SQL2-CLS** `1dd9b9b1` (SIGN-CLS-2; one commit, `classes.json` only): `atx.sign-receipt/v1` joins class
  **`ic-output`** (O4) `schemas`; `writer` gains "atx-equity-strategy-ic sign (strategy_sign_verb, P9 SIGN)",
  `pinned_by` gains "the screen receipt's sign.receipt_sha256 (P9 SIGN)". Classes guard before **1 failed / 4 passed**
  (only `atx.sign-receipt/v1`), after **5 passed** (97 subtests); store suite **43 passed** at seeds 0 and 1; fixture
  chain `catalog_digest d32f7655...4e69` unchanged (rel and abs), `verify --pins` 0, identity 12 / 0.
- `git merge --no-ff 50a43599` (the deletion, only after the identity below) -> **`338e6a7c`**, 0 conflicts, tree
  `3337fd2e` = the recompute. The four files' merged blobs equal the lane's tested blobs (`wave_rules.py` `71a23ba3`,
  `test_research_wave.py` `4c1c8393`, `test_wave_sign_verb.py` `2b60956e`; `test_no_python_mirror.py` auto-merged with
  root's D3W-PB side, keep-both PASS against the merge base `c2010ed1`; `sign-keep-both-50a4.txt`).
- `git merge --no-ff e2844926` (the review record) -> **`cdc753a8`**, 0 conflicts, tree = the recompute.

### Identity (C3-11; `sign-logs/sign-identity-ledgered.py`, blob `dddd1140` = pool-29's at `c2010ed1`)

On `1dd9b9b1` with `wave_rules.sign_pm7_35` present and the p9-3b IC exe `ef3974cf...` (source `1dd9b9b1`, disk =
receipt), over the four tracked, unmodified ledgered results `.superpowers/sdd/platform-v8-20260929/waves/{y-2,y-3,y-5,y-s}/wave-result.json`
by path: Y-2, Y-3, Y-5 are rule waves (screen null: listed, skipped); **Y-S, rule `pm7-35`, 15 rows: verb == recorded 15
/ 15, verb == Python rule 15 / 15, kept / dropped equal, the receipt pins the rows: IDENTICAL; `RESULT PASS`**, exit 0
(`sign-identity-ledgered-3b.log`). The tool reads only `screen` (rows, kept, dropped, sign_rule) and prints counts and
ids; no statistic was read. On PASS the deletion was merged.

### Tests (ENV-1: 0 hits; `ATX_EQUITY_IC_EXE` = the p9-3b IC exe)

| suite | result |
|---|---|
| `SignVerb*` (p9-3b IC tests) | **6 / 6** |
| pre-deletion: `test_wave_sign_verb.py` (real exe) | **21 passed, 0 skipped** (the 24-row identity against the Python rule ran) |
| pre-deletion: `test_research_wave.py`; `test_wave_driver.py` | **19 passed**; **28 passed** |
| pre-deletion: the other wave files that run `wave_fixture` (`test_wave_gross_match`, `_review`, `_review_minor`, `_scoreboard`, `_speed`, `_hardening`, `_queue`, `_eval_verb`, `_kinds`, `test_research_store_hook`) | **269 passed, 1 skipped** (the skip: `test_wave_kinds.py:1406`, "ATX_EQUITY_BIN does not name a bin dir with both exes": the named skip M4 also saw, 191 / 1) |
| pre-deletion: `test_no_python_mirror.py` (own invocation) | **7 passed**, G-P5 1 function in 1 row |
| post-deletion: `test_wave_sign_verb.py` (real exe); `test_research_wave.py`; `test_no_python_mirror.py` | **21 passed**; **19 passed**; **7 passed**, "G-P5: 0 mirrored rule function(s) in 0 row(s)": the count dropped by exactly 1 function / 1 row |
| post-deletion freeze state (`mirror_freeze.py`) | rows left for P9 lanes at the freeze: **0** (with `P9_FREEZE = True` the freeze test passes); `FROZEN_PAIRS` still names `sign_pm7_35`; `sign_pm7_35` and `screen_decision` are gone from `scripts/wave_rules.py` |

### Root items (`task-SIGN-review.md` "What root must check at merge")

1 (pre-deletion merge, keep-both, SQL2-CLS, build, `SignVerb*` 6, the wave files) **holds**; 2 (identity by path)
**holds** (`RESULT PASS`; no refusal); 3 (deletion only on PASS; re-runs; count -1 / -1; `FROZEN_PAIRS`) **holds**; the
C3B-S relabel is moot (the row is gone); 4 EXE-FREEZE: the IC exe's bytes changed (p9-3b); every exe linking
`atx-impl-strategy` (NAV `atx-equity-strategy-targets`, `atx-equity-strategy`, `-risk`, `-mine`, `atx-impl`) is now stale
by source and relinks at the gate's builds, before the P9-B0 re-pin; 5 stretch: APPROVE and merged; SIGN-SUBST: no
substitution entry; 6 the whole IC target, Release, seeds 0 / 1, stage chain: the gate's.

### For PRE (round 2b-ii)

SIGN merged: per SIGN-SUBST the prereg's B.0 "Manifest" names the screen stage's sign receipt (`atx.sign-receipt/v1`,
class `ic-output`, `out/waves/<w>/sign/receipt-<k>.json` with `rows-<k>.json`) as a per-wave file the judge pins (the
PRE 2b-ii list's SIGN-SUBST line, now unconditional). G-P5 at the freeze: 0 rows of P9 lanes left.

## A4

### Merge (pre-flip head only)

- `git merge --no-ff 6dab6948` (pre-flip head; code `28455a9a`) -> **`bba4c06f`**, **0 conflicts**, tree `d969238a` =
  the recompute. Merge base `73b4acfd`. 75 code / fixture files (+5,382 / -63): the session clock, the short-side stage
  readers, the FTD / Reg SHO builders and their kinds in `atx-engine-research-fields`, the CLI / reuse changes (A3 S-2,
  A3-3), the holdings fixture and its tests, the A1-SHIM shims. `atx-engine/CMakeLists.txt` auto-merged with COV-MV's
  `mv_target.cpp` line: keep-both PASS (`a4-keep-both.txt`). Flag absent: both registry rows stay `kind: python`.
- **Not merged:** the flips `69141218` (ftd), `59d807ea` (regsho) and the APPROVE head of record `fc3e414e` (which
  contains them). A4-Q2's TRAIN identity did not run (below).

### Build and tests (p9-3c, source `bba4c06f`; ENV-1: 0 hits; `ATX_RESEARCH_FIELDS_EXE` = the p9-3c fields exe)

| suite | result |
|---|---|
| A4 filter `ResearchFieldsA4*:ResearchFieldsHoldings*:ResearchFieldsRegistry*` | **25 / 25**, incl. `ResearchFieldsHoldingsFixture.FtdIsThePythonField` and `.RegshoIsThePythonField` |
| `test_holdings_fixture.py` (own session) | **2 passed** (the committed fixture is the Python builder's output, re-run) |
| `test_holdings_engine_path.py` (own session) | **2 passed, 0 skipped** (`test_flag_absent_is_the_plain_builder`, `test_real_executable_identity`) |
| `test_vendor_engine_path.py`; `test_research_fields_engine_path.py`; `atx-engine/tools/test_field_registry.py` (own sessions) | **10 passed**; **11 passed**; **37 passed** (8 subtests) |
| classes guard (SQL2-CLS: no new literal) | **5 passed** |

The fixture identity of record (`FtdIsThePythonField` / `RegshoIsThePythonField` + `test_holdings_fixture.py` +
`test_holdings_engine_path.py::test_real_executable_identity`) **holds**.

Registry rows (`a4-registry-rows.txt`; item 6): at `bba4c06f` both fields `kind python`, builder
`research_fields_holdings`, owner gains the suffix `; engine-shim:prepare_research_fields_engine`, formula pins unchanged
(`cce14201...`, `7e869d7d...`); `kind engine` / builder = the field name only in the flips; `coskew_60m`, `xrd0_ttm`
`kind python` at every revision. **Holds.**

### A4-Q2 TRAIN identity: NOT RUN (stop at the lane boundary; `m5-logs/a4-q2-train-identity.txt`)

- Case: BOTH fields are listed in Y-F0's `--fields` manifest `build-equity/train-2020-2023-lo3-fields-v15/manifest.json`
  (sha256 `26fee5ce...`; payload pins `ftd_shares_ratio21.f64` `15bc4e6a...`, `regsho_threshold_days63.f64` `8248b3a2...`;
  both reused from `-v15a`), so per A4-Q2 the identity on the real TRAIN inputs is required before the flips.
- The TRAIN recipe that wrote the manifest (`train-2020-2023-lo3-fields-v15-run/receipt.json`, argv only) reads its
  holdings stages from `C:/atx/atx-db/data/alpha_panel/v1/{ftd, regsho_threshold, security_master}` (pins `a76d69be...`,
  `fb073c62...`, `3afe0660...`) and most of its other inputs from `C:/atx/atx-db/data/`. No copy exists outside atx-db.
  The owner rule "never touch `atx-db/`" (and DATA-1's "`C:/atx/atx-db` is never opened") forbids opening them; another
  session runs jobs there. Root opened no file under `C:/atx/atx-db`, did not start the bounded runner and did not merge
  the flips (a stop for "anything outside the worktree", contract; r11 step 5's "a miss STOPS A4's merge" form).
- State: **A4 BLOCKED for the flips.** The merged pre-flip head moves no byte of any P9 run (flag absent). The PM / owner
  rules whether the identity may read those stages (then a root group runs `task-A4-report.md` "For root" item 3 and
  merges `69141218`, `59d807ea`, then `fc3e414e` or the fix-1 head) or the flips go to P10.

### Root items (`task-A4-review.md` "What root must check at merge")

1 (pre-flip merge, build, filter 25, the lane's pytest files, fixture identity) **holds**; 2 (TRAIN identity on both)
**not run: BLOCKED** (above); 3 (flips) **not merged**; 4 (manifests after the flips) moot until the flips; 5 SQL2-CLS
**holds** (none; guard 5); 6 registry rows **hold**; 7 the gate's (Release, whole fields / engine targets, seeds 0 / 1,
stage chain: `prepare_research_fields_engine.py` changed).

### For PRE (round 2b-ii)

The A4-Q2 case: both flipped fields are in Y-F0's manifest; the TRAIN identity is NOT established in M5 (inputs under
`C:/atx/atx-db/data/`, owner rule); the flips are not in root, so every P9 cell builds or reuses the two fields with
the Python builder (flag absent). The prereg states the case and "identity pending / not run" unless a later root group
runs it. (Superseded by M5c below: "identity not run: inputs under atx-db; flips P10".)

### M5c (rulings A4-DB, A4-P10-FLIPS; the same root agent, continued; read-only, no build, no test)

The PM ruled A4-DB (NO: `C:/atx/atx-db/` is never opened) and ordered a READ-ONLY search for a copy of the two fields'
TRAIN inputs outside atx-db, the identity only on a SHA-matched copy with year partitions <= 2023, else A4-P10-FLIPS.

1. **Provenance** (`m5-logs/a4-provenance.txt`, from the `sources` and `reused_from.inputs` of the two entries of
   `build-equity/train-2020-2023-lo3-fields-v15/manifest.json`, `26fee5ce`; both reused from `-v15a` with the same pins):
   - `ftd_shares_ratio21`: `C:\atx\atx-db\data\alpha_panel\v1\ftd\manifest.json` (`a76d69be...`), its six
     `year=2018 .. year=2023\ftd.parquet` files (each SHA-256 recorded), and `shares_out.f64` (`1cff53f8...`,
     `build-equity/train-2020-2023-lo3-fields-v9`, in pool-2).
   - `regsho_threshold_days63`: `C:\atx\atx-db\data\alpha_panel\v1\regsho_threshold\manifest.json` (`fb073c62...`),
     `lists.parquet` (`4ebd1881...`, not year-partitioned), six `year=2018 .. year=2023\threshold.parquet`;
     `C:\atx\atx-db\data\alpha_panel\v1\security_master\manifest.json` (`3afe0660...`) and `finra_names.parquet`
     (`1acfed75...`, not year-partitioned).
2. **Search for a copy outside atx-db** (`m5-logs/a4-copy-search.txt`; names and sizes only, no file opened; lister
   `m5-logs/tools/find_inputs.py`, an `os.walk` over names): the whole pool-2 tree including `build-equity/`: only the
   A4 synthetic fixture under `atx-engine/tests/fixtures/research_fields_holdings/` (4,153 / 7,365 / 2,660 / 1,853
   bytes, not the recorded inputs); `C:/atx/data`: 0 hits; `data-classification.md`: no such entry; the lane pools 13,
   14, 15, 20, 21, 22, 23 (root only; its `build-equity/` not listed, TEETH-BIN-23), 27-32 by `ls` of the pool root and
   its `build-equity/` top level: 0 hits; pools 8 / 24 (frozen) and 25 / 26 (other sessions) skipped. Slip, logged: the
   first two `find` passes were rewritten by the `rtk` hook and printed nothing; they are void, and the `os.walk`
   lister replaced them.
3. **Outcome: no copy exists outside atx-db, so nothing was hashed and the identity did not run. A4-P10-FLIPS applies:**
   A4 stays merged at `6dab6948` (merge `bba4c06f`: the engine builders, kinds, tests and fixture are in, flag absent);
   the flips `69141218`, `59d807ea` and the registry rows of `fc3e414e` are P10 whole; Python stays the producer of record
   for both fields; no byte of Y-F0 moves; nothing reverted. Even with a copy, the two non-partitioned files
   (`lists.parquet`, `finra_names.parquet`) could not have been opened under the 2024 rule.
4. **For PRE (round 2b-ii):** the prereg's A4-Q2 entry reads "both fields are in Y-F0's manifest; identity not run:
   inputs under atx-db; flips P10". A4 fix 1 follows A4-FIX1-CHAIN (re-applied on `6dab6948` iff a plain cherry-pick;
   the PM's at the lane's hand-back).
5. Blindness (disclosed): printing the two fields' manifest entries to find their `sources` also printed their
   `coverage` blocks (cell counts, finite fractions and value quantiles of the two fields, 2018-2023): descriptive field
   counts, no return / IC / Sharpe / turnover / NAV statistic; nothing of it is recorded (`a4-provenance.txt` carries
   paths, bytes and SHA-256 only).

### Disk and status (M5)

- Root's object trees `build-equity/` and `build-equity-rel/` kept (the gate builds Debug and Release there; the current
  Debug exes listed in `handoff-root-m5b.md` are the gate's inputs). The receipts and build logs of p9-2z, p9-3a, p9-3b,
  p9-3c are in `build-equity/` with copies in `m5-logs/`.
- Lane pools: nothing deleted (pool-23's `build-equity/` untouched, TEETH-BIN-23).
- Session scratchpad `root-m5/` (basetemps, the D3W-PB identity's BASE archive and outputs, the SIGN identity's rows /
  receipts, the fixture-chain catalogs): mine, regenerable; deleted by literal name at the end of the group.
- Code head **`bba4c06f`**. Build tags used p9-2z, p9-3a, p9-3b, p9-3c; **next free build tag p9-3d**. Known-red set:
  **EMPTY** (B2 fix 2 cleared `test_legacy_n37_numbers_reproduced`); no new red in M5. Trial ledger 133 lines
  `27e40f9f` at start and end (0 trials). SQL3-2 holds at start and end. No lease touched. No permission refusal.
- Blindness (disclosed): one `git grep` for the two n37 golden SHA-256 values (looking for root's own record of them)
  printed lines of the tracked `docs/plans/2026-09-29-mega-alpha-v7-pitch.html`, one carrying a v7.1 in-sample figure
  (the same public v7 document the B2 re-reviewer hit); nothing of it is recorded or used; later searches used file
  names only. Reading the v15 fields manifest's `source_checks.holdings` block printed source row counts and vendor
  short-volume coverage ratios (data-quality counts, no return / IC / Sharpe / turnover / NAV statistic); none recorded.

## Step group M6 (RESUME 11): AL-CLOCK fix 1, SQL3 item 5

Root M6, 2026-10-15 UTC (ledger "Dispatch root M6"; `r11-dispatch.md` "ROOT M6"; start state `handoff-root-m5b.md`:
code head `bba4c06f`, docs head `97505c1c`, next free tag `p9-3d`, known-red set EMPTY). Evidence: `m6-logs/` (tools
`m6-logs/tools/`: `build.sh`, `busy.ps1`, `gt.sh`, `pyt.sh`, `warn_count.py` copied from `m5-logs/tools` with the logs
dir changed; new: `cap_decl.py` (the text of `capacity_declaration` in `strategy_nav_v7.cpp` at two commits, by
SHA-256) and `exe_sha_end.py` (per exe, the newest Debug receipt and whether the bytes on disk equal it)). Exit tables
`m6-logs/gt-exits.txt`, `pyt-exits.txt`, `builds.txt`; per-merge checks `alclock-checks.txt`, `sql3-checks.txt`; start
and end `start-state.txt`, `end-state.txt`; exe hashes at the end `exe-sha-end.txt`. Handoff: `handoff-root-m6.md`.
TEST-1 / ROOT-1a throughout: targeted checks at each merge; whole suites, Release, canaries, golden and the parents'
flag-absent identity runs are the gate's.

Start state (`m6-logs/start-state.txt`): HEAD `97505c1c` (docs), code head `bba4c06f`; the only tracked change is the
PM's `progress.md`; five untracked `docs/plans/*.png` (not root's, never added); trial ledger 133 lines `27e40f9f`;
`build-equity/research-store/` absent (SQL3-2); the five exes this group uses or replaces hashed equal to their
receipts (p9-2y target-tests and NAV, p9-2r store, p9-2z eval, p9-3b IC); no build of `C:\atx-wt\` running; free RAM
about 3.5 GB, C: 57.7 GB free.

MERGE-SHA at each slot: AL-CLOCK `eed512bd` (ledger "Task AL-CLOCK: APPROVE at `eed512bd`"), SQL3 `22b8fd17` (ledger
"Task SQL3: APPROVE at `22b8fd17`"). Both merged whole, 0 conflicts, no slip.

The one build of the group (Debug `equity-dev` through `scripts/research-build.ps1`, target-scoped, no `-Canary`):

| tag | source | targets | TUs / links | wall | exes (disk = receipt at every run and at the end) |
|---|---|---|---|---|---|
| **p9-3d** | `1d003241` (the AL-CLOCK merge) | `atx-impl-strategy-target-tests,atx-equity-strategy-targets` | 5 / 3 | 24 s (32 s with the wrapper) | target-tests `685b8ba2947f878a9aa0a6a77cd7e4639c83588efc1cb398519be735efb13454`; NAV `7e101b564d800c9e4b57541e11a30d17d8f32d5f74fd2c5ccc95d3b2e5b5f0dc` |

Exit 0, 0 warning lines; code dirty 0 (DirtyEntries 6 = `progress.md` and the five PNGs); ConfiguredProvenance
`bba4c06f` (no CMake file changed since that configure); admitted at once (no other build of `C:\atx-wt\`; free 3628
MiB, commit 7633 MiB; HOST-4 / HOST-5).

## AL-CLOCK fix 1

### Merge

- `git merge --no-ff eed512bd` (re-review 1 APPROVE of fix head `f892eab1`; ruling ALCLOCK-S; MERGE-SHA; pool-30
  branch `feat/p9-alclock2-20261007`, never pool-8's) -> **`1d003241`**, **0 conflicts**, tree `91d2526a` = the
  recompute (`git merge-tree --write-tree 97505c1c eed512bd`). Merge base `5c811b56`. The first AL-CLOCK merge
  (`e9b3a323`) is `858a4fca` (M4), so this one brings only fix round 1.
- 7 code files: `atx-impl/src/book/nav_cli.cpp`, `atx-impl/src/book/nav_lockstep.cpp`, `atx-impl/src/strategy_nav_v7.cpp`,
  `atx-impl/tests/fixtures/nav_rule_info_clock_rate_v1.json` (new), `atx-impl/tests/strategy_nav_info_clock_test.cpp`,
  `scripts/specs/p9/rules/info-clock-rate-v1.json`, `scripts/tests/test_info_clock_rule.py`. Records arrived with it
  (`task-AL-CLOCK-rereview-1.md`, the report's "Fix round 1 (ALCLOCK-S)", `handoff-AL-CLOCK.md`, `alclock-logs/` with
  the `-k` / `-l` / `-m` receipts).
- W3-9: the merge touches neither `atx-impl/tests/CMakeLists.txt` nor a registry / parser tail (COV-MV has been in the
  lane's branch since `38967882`): nothing to keep both.

### Tests (p9-3d; ENV-1: 0 hits)

| suite | result |
|---|---|
| `InfoClock*` | **14 / 14** = M4's 13 + `InfoClockRate.CalibrateGrossKeepsTheRatesAndCountsOnce` (the calibrate gtest of S-3); InfoClockRate 10, InfoClockPrint 4 (`gt-infoclock.log`) |
| the lane's named filter `InfoClock*:NavSpec.*:NavRules.*:TradeRateRegistry.*:NavAnnMeanGross.*:CostLawRegistry.*:TargetRegistry.*` | **26 / 26** = M4's 25 + the calibrate gtest (`gt-alclock-named.log`) |
| `scripts/tests/test_info_clock_rule.py` | **5 passed** at seed 0, **5 passed** at seed 1 |
| Debug whole target-tests, Release (named, whole), canaries, whole `scripts/tests`, the P9-B0 parents' flag-absent identity and the opt-in check | deferred to the gate (TEST-1) |

The counts equal the lane's and the reviewer's (`InfoClock*` 14, named 26 at `f892eab1`). The dispatch's "`InfoClock*`
24 + 1" matches no record; the "+ 1" is the calibrate gtest, which is in both counts above.

### Root items (`task-AL-CLOCK-rereview-1.md` "What root must check at merge (M6)")

| item | result |
|---|---|
| 1 merge at the review commit; 7 files + records; not a keep-both step | **holds** (above; the reviewer's predictive merge-tree was against `138e1d2c`, root's head at the merge was `97505c1c`: clean) |
| 2 the named filter on a fresh Debug target-tests | **holds**: 14 / 14 and 26 / 26 on p9-3d (the lane's object trees are deleted, and root's p9-2y target-tests and NAV were stale by source: EXE-CUR) |
| 3 `test_info_clock_rule.py` at seeds 0 and 1 | **holds** (5 / 5 each) |
| 4 the registration sentence (ALCLOCK-4) | **holds**: `scripts/specs/p9/rules/info-clock-rate-v1.json:51` carries "planned gross <= (theta_f / theta_s) x gross(aim) by contraction outside the dust band, 2.589x at cadence 1" and "realised gross leverage is read from the cell's own summary (max and mean over the cell) and printed beside the parent's", then "the OD5-7 cap test applies to that realised figure"; the superseded clause is absent; SHA-256 of the HEAD blob = of the working bytes = `7801c19880a225a2f4c6ecb8756117f03fccafa71b92f440830bfb6fdcb8a152`; template `scripts/specs/p9/templates/info-clock.json` `95d4104a...42d5` unchanged; pinned nowhere yet (the W1-I2 re-pin at the gate reads the new SHA) (`alclock-checks.txt`) |
| 5 flag absent, byte for byte (the capacity declaration conditional) | **holds** by reading and by test; byte identity of the P9-B0 parents is the gate's (TEST-1). `capacity_declaration`'s text is the same at `97505c1c` and `1d003241` (`cap_decl.py`: SHA-256 `611d4725...a828` at both, 553 characters); `capacity_rule(false)` returns it; `State::info_clock` defaults to false and is set only for `NavRateRule::InfoClockV1`; the recipe selects by its own `rate` key (written only under the rule). `CapacityCurveAcceptsThisRateOnly` (a fixed-rate capacity run keeps "; fixed rate only; x1 is S2 bit for bit" and names no info-clock) and `InfoClockPrint.FlagAbsentLoadsNoClock` pass. Changed by design: the `--help` text (two clauses) and two refusal messages of runs that write nothing |
| 6 the row fixture against the merged registry | **holds** (`InfoClockRate.RegistryRowAndParser` passes, in the 14) |
| 7 ledger | **holds**: "Task AL-CLOCK: APPROVE at `eed512bd`" is in the ledger, so `eed512bd` is the SHA of record |
| SQL2-CLS | **holds**: no `atx.<name>/v<n>` literal in the added lines; nothing for `classes.json` |

### For PRE (round 2b-ii, Appendix IV items 16 (B.4 P9-C) and 31 / 33 (F.1, Appendix III))

- Item 16, replacing the pre-fix values M4 recorded: registration `scripts/specs/p9/rules/info-clock-rate-v1.json`
  sha256 **`7801c19880a225a2f4c6ecb8756117f03fccafa71b92f440830bfb6fdcb8a152`** (was `51b8b754...e33c`, merged at
  `858a4fca`); template `scripts/specs/p9/templates/info-clock.json` sha256
  `95d4104a421c32f91877649f1ec9dfbdb409c9a121d6f6e2377e5e25e19b42d5` (unchanged). The registration's
  `realised_leverage` carries ALCLOCK-4's two clauses verbatim (the P9-C block carries the same, ALCLOCK-4). The
  ALCLOCK-S2 S-A disclosure (the capacity sentence's "x1 is S2 bit for bit" is tested on fixed-rate runs only; the
  rule-on test is P10) stands as the ledger's PRE 2b-ii line. Rule, row, producers, C3-4, DEC-16 argument and the C3-5
  probe as M4 recorded; fit mode at the wave-3 P9-B0.
- Items 31 / 33: `source_sample_end` 2019, unchanged by the fix round.

## SQL3 item 5

### Merge

- `git merge --no-ff 22b8fd17` (review 2 APPROVE of `0ebb83da`; `22b8fd17` = `0ebb83da` plus the review file only;
  code `00779453`; MERGE-SHA) -> **`d51d011e`**, **0 conflicts**, tree `6c73537a` = the recompute
  (`git merge-tree --write-tree 1d003241 22b8fd17`). Merge base `175edd61`.
- 8 Python files, no C++, no CMake, no `classes.json`: `scripts/cycle_resume.py`, `scripts/cycle_verdict.py`,
  `scripts/wave_stage_record.py` (item 5), `scripts/wave_queue.py` (SQL3-WQ, both hunks, SQL3-WQ-2),
  `scripts/tests/test_research_store_hook.py`, `scripts/tests/test_wave_queue.py`,
  `atx-engine/tools/test_research_store_identity.py`, `atx-engine/tests/fixtures/research_store/make_store_tree.py`.
  The review's "nine Python files" lists these eight (a count slip in the review; no file is missing). No build.
- Records arrived with it (`task-SQL3-review-2.md`, `task-SQL3-review.md`, the report's "Item 5 for the follow-up",
  `handoff-SQL3.md`, `sql3-evidence/`).

### Tests (head `d51d011e`; seed 0; ENV-1: 0 hits)

Env: `ATX_EQUITY_ROOT=C:/atx-wt/pool-2/build-equity`, `ATX_EQUITY_BIN=C:/atx-wt/pool-2/build-equity/bin`,
`ATX_RESEARCH_STORE_EXE=<that bin>/atx-research-store.exe`; each file (and the writers' set) in its own invocation,
`-p no:cacheprovider`, own `--basetemp`, `PYTHONDONTWRITEBYTECODE=1`. EXE-CUR (`sql3-exe-sha-before.txt` =
`sql3-exe-sha-after.txt`): store `d8a500d8...9d65` = p9-2r (source `fd084e78`; the store sources are unchanged since,
current by source), eval verb `47910191...893b` = p9-2z (the writers' `nav_summ` cases); NAV p9-3d and IC p9-3b hashed
only (no file of these runs names either exe).

| file(s) | result |
|---|---|
| `scripts/tests/test_research_store_hook.py` | **13 passed** (`test_real_store_exe_ingests_every_class` PASSED: the real-exe case ran) |
| `atx-engine/tools/test_research_store_identity.py` | **16 passed, 6 subtests passed** (`FixtureChain::test_cpp_catalog_renders_identically` PASSED: the real exe ran; the `-rA` run prints no subtest count, the `-q` re-run does: `pyt-sql3-identity-q.log`) |
| `scripts/tests/test_wave_queue.py` | **6 passed** (the relative-root case PASSED) |
| the seven writers' files (`test_cycle_resume.py`, `test_cycle_scoring.py`, `test_research_cycle_roles.py`, `test_research_wave.py`, `test_wave_driver.py`, `test_wave_hardening.py`, `test_wave_eval_verb.py`) | **98 passed, 0 failed, 0 skipped** |
| whole `scripts/tests` seeds 0 / 1, the stage chain, the rest of the store suite | deferred to the gate (TEST-1) |

### Root items (`task-SQL3-review-2.md` "What root must check at merge (M6)")

| item | result |
|---|---|
| 1 merge (root had moved past `bba4c06f`: only AL-CLOCK's merge, no sprint-dir or Python path in common) | **holds** (0 conflicts) |
| 2 Python only; store exe current | **holds** (8 Python files; p9-2r `d8a500d8` = receipt) |
| 3 the three test files + the seven writers' files | **holds** (above) |
| 4 one ingest per write | **holds**: the tests that pin the counts pass on the merge head (fake wave 13 ingests, 13 warnings with a failing store; `write_binding` 1; `write_verdict` 1, 2 with `keep`; `keep_copy` 1 per copy; the SQL3-WQ case 3); no wave fixture changed between the review's trial merge (`bba4c06f`) and root's head (AL-CLOCK's only Python file is `test_info_clock_rule.py`) |
| 5 SQL3-2 | **holds**: `build-equity/research-store/` absent at the start, before and after the runs, and at the end |
| 6 SQL2-CLS | **holds**: no `atx.<name>/v<n>` literal in the added lines; `classes.json` not in the merge |
| 7 the freeze print | the gate's: G-P10 (d) PARTIAL ("three writers hooked, the stage record's side files not", SQL3-S2 S-2), (e) landed for `atx.run-store/v1`, printed from the merged tree, not the lane report (SQL3-S2 S-3); review 1's F-1 strip for console byte comparisons (S-4) |

SQL3-S2 S-1 and S-4 (the rest of review 1's Suggested) are P10, as ruled.

### For PRE (round 2b-ii)

None. No Appendix IV placeholder names SQL3 (as at its first merge); the record hook stays off for every run of record
(SQL3-2), so no P9-B0 reference or cell needs a `store` entry on a substitution list.

### Disk and status (M6)

- Root's object trees `build-equity/` and `build-equity-rel/` kept: M6b (the A4 re-applied fix head's Debug build, SQL4)
  and the gate's Debug and Release builds use them. The group created under `build-equity/` only the p9-3d receipt and
  logs (copies in `m6-logs/`) and relinked the two p9-3d exes.
- Session scratchpad: `root-m6/` (pytest basetemps) and four extracts of lane records (`alclock-report.md`,
  `alclock-rr1.md`, `sql3-report.md`, `sql3-rv2.md`), all mine and regenerable; deleted by literal name after the
  records commit.
- Lane pools: nothing deleted (pool-30 holds no object tree; pool-13's was deleted by the lane, SQL3-TREE; pool-23's
  `build-equity/` untouched, TEETH-BIN-23).
- Code head **`d51d011e`**. Build tag used p9-3d; **next free build tag p9-3e**. Known-red set: **EMPTY**; no new red.
  Trial ledger 133 lines `27e40f9f` at start and end (0 trials). SQL3-2 holds. No lease touched. No permission
  refusal. No data opened, no file under `C:/atx/atx-db` opened, `holdout_gate.py` not run; no statistic printed or
  recorded (the "2.589x at cadence 1" quoted above is the registration's mechanical rate-ratio bound, not a measured
  figure).
