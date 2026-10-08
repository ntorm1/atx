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
