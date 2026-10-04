# MAIN-V: verification of the main merge `dec03454` (pool-25, 2026-10-04)

Merge `dec03454` = `feat/platform-p9-20261003` @ `4cd39925` into main `179af41c` (first parent main). Worktree
`C:/atx-wt/pool-25`, branch `merge/p9-into-main-20261003`, head = local `main` = `dec03454`, tree clean. Resumed from the
interrupted session's receipts (all re-checked by SHA); nothing committed, no ref moved, lease kept.

## Verdict: RED, not caused by the merge

- **The merge is clean where the two sides meet.** Each side's change lands verbatim, the one conflict is resolved
  correctly, and every seam suite is green (detail below).
- **Merged main is red in `atx-engine-factory-tests`:** 6 gtests, none in the known-red set. By reading, all 6 come from
  **main's side (`179af41c`)**, not from the sprint and not from the merge. They are deterministic (same values on a
  rerun and from a second target). Undoing local main would not remove them.
- The known-red pytest `test_fields_are_the_rule_applied_to_the_registry` is red as expected. Release
  `BookNormalScore.*` x2 was not run: Release was not built here (Debug `equity-dev` only).

## Evidence

All build receipts: `Source` = `ConfiguredProvenance` = `dec03454`, `DirtyEntries` 0, `ExitCode` 0, preset `equity-dev`,
`scripts/research-build.ps1`. Executable SHA-256 values were re-hashed this session and match their receipts. Logs and receipts are in
`C:/atx-wt/pool-25/build-equity/`.

| target | tag | exe sha256 | tests | result | log |
|---|---|---|---|---|---|
| `atx-impl-tests` | main-a (prior session; 407 TUs, 554 s) | `364c86ab` | whole, 1,131 | **1,126 passed / 5 skipped / 0 failed** | `main-a-impl-tests-whole.log` |
| `atx-impl-tests` seam filter | main-a exe, rerun this session | `364c86ab` | `AtxImplProvenanceDigest.*:AtxImplProvenance.*:AtxImplStoreDiscover.*:AtxImplDiscover.*`, 57 | 56 passed / 1 skipped (W6, same as whole) | `mainv-impl-discover-provenance.log` |
| `atx-impl-strategy-ic-tests` | main-b (14 TUs, 16 s) | `7c50c08f` | whole, 193 | **193 passed** | `main-b-strategy-ic-tests-whole.log` |
| `atx-engine-research-fields-tests` | main-c (34 TUs) | `c14df0eb` | whole, 66 | 66 passed | `main-c-research-fields-tests-whole.log` |
| `atx-engine-factory-tests` | main-d (49 TUs, 128 s) | `1aaa23a2` | whole, 401 | **395 passed / 6 FAILED** (below) | `main-d-factory-tests-whole.log`; rerun of the 6 with identical values: `mainv-factory-reds-rerun.log` |
| `atx-engine-equity-review-tests` (main's own target, from main's hunk in the merged `tests/CMakeLists.txt`) | **mainv-a** (17 TUs, 1 link, 39 s, -j3, 0 warnings) | `9c06110b` | whole, 203 | 198 passed / **5 FAILED** (the same 5 non-NSGA reds, identical digests) | `mainv-a-equity-review-tests-whole.log` |
| same, main's recorded review filter (`atx-engine/plans/2026-10-01-equity-review-sprint.md:72`) | mainv-a | `9c06110b` | 58 | **58 passed** | `mainv-a-equity-review-mainfilter.log` |
| pytest `scripts/tests` | prior session (invocation not logged) | - | 378 | 373 passed / 4 skipped / 1 failed = known red (`:119`, same three names `noa_lag4`, `ea_window_pre5`, `ins_cluster_buy`) | `main-pytest-scripts-tests.log` |
| pytest `atx-engine/tools` (excluding `test_stage_chain.py`) | this session | - | 442 | 441 passed / 1 skipped (`atx-research-store.exe` not built) / 135 subtests | `mainv-pytest-engine-tools.log` |
| pytest `atx-engine/tools/test_stage_chain.py` (separate invocation) | this session | - | 9 | 9 passed | `mainv-pytest-stage-chain.log` |

pytest flags: explicit paths, `-B -q -rs -p no:cacheprovider`, `PYTHONDONTWRITEBYTECODE=1`, own `--basetemp` (deleted).

- The prior session's `scripts/tests` count equals the sprint M1d baseline exactly (373/4/1). Its 7.5 h wall time
  looks like the host slept; it is not a hang.
- The prior session's `atx-engine/tools` run (450 passed) had `test_stage_chain.py` in the same invocation:
  441 + 9 = 450. So I re-ran the two separately.

## Where the two sides meet

- **Shape:** merge base `4e0aa98f`. Main side: 76 commits, 244 files. Sprint side: 497 commits, 939 files.
- **Intersection (4 files):** `search_driver.hpp`, `search_driver.cpp`, `atx-engine/tests/CMakeLists.txt`,
  `atx-impl/src/stage_discover.cpp`.
- **No Python file is on both sides.** The sprint Python that imports `atx_db` (`prepare_research_fields.py`) uses only
  `research.spine`, `migrations.bodies_0327` and `reference_classifications`, none of which main changed. No pytest
  drives the discover manifest.

**Auto-merged files:**

- The three files are an exact union: each side's `git patch-id --stable` is identical before and after the merge.

  | file | main side | sprint side |
  |---|---|---|
  | `search_driver.cpp` | `46c567fd` | `6d79e3b2` |
  | `search_driver.hpp` | `59223b21` | `b1d697d2` |
  | `tests/CMakeLists.txt` | `d870ac15` | `f72af512` |

- The sprint's `search_driver` hunks (MINE-MEM, MINE-16) are confined to the signal-fitness path. They are gated by
  `signal_on = cfg.signal_fitness != nullptr` or by the signal evaluator. None of the failing test files uses
  `signal_fitness`. On the legacy path the merged driver is main's code.

**The conflict, `stage_discover.cpp`:**

- The merged file is the sprint file plus main's 5 hunks, verbatim, and nothing else differs. Main's hunks:
  `mine_research_into`, plus `admission_evidence=research_only` in both manifests and both `kvs`.
- The sprint's `_config.json` sidecar is kept, so `AtxImplProvenanceDigest.ConfigJsonNotInDiscoverDigest` (red on the
  sprint side since 2026-09-26) is **green** on the merge, and
  `AtxImplProvenance.ConfigSidecarHoldsTheRunConfigAndStaysOutOfTheManifest` is green.
- Gap on main's side: no test asserts the `admission_evidence` line.

## The 6 factory reds (outside the known-red set)

| test | failing check (output) | side | why |
|---|---|---|---|
| `NsgaSearch.DeflateSelection_GenZeroRawIdenticalToOffPath` | `factory_nsga_search_test.cpp:652`: gen-0 best raw ON 9.1866137815 vs OFF 9.1866139131 (diff 1.316e-7 > 1e-12) | **main** (`40357c1f`) | `40357c1f` sets `gen_fit.trial_count = max(1, prior + canon.size() + n_to_score)`, so gen-0 N > 1. The test (not changed on either side) requires N = 1 at gen 0, and its own failure message names that rule. |
| `DeflateSelection.PriorTrialCountDeflatesSearchSelection` | `deflate_selection_running_n_test.cpp:198`: `EXPECT_NE(r_n0.digest, r_big.digest)`, both 13924069305505703040 | **main** (`40357c1f`) | The test is unchanged on both sides, and the selection code is main's. The owner's uncommitted WIP in `C:/atx` (branch `feat/tier1-v3-warehouse`, which has `40357c1f` and no sprint commit; file blob = `179af41c`'s) deletes exactly this assertion. |
| `DeflateSelection.VersionedCheckpointRefreshesWithoutRepeatingBacktests` | `:378` full digest 11311877773886515565 vs resumed 15004366510931238125; `:380` candidate order differs from index 6 | **main**, by reading (least certain of the six) | Main added the test in `40357c1f`. Main's recorded filter excludes `DeflateSelection.*`, and its plan says "No full suite". The same `C:/atx` WIP adds per-generation population / `n_evaluated` diagnostics to exactly this test on a sprint-free tree. |
| `FactoryOos.R3b_DigestUnchangedByPbo` | `factory_oos_test.cpp:923` digest 7014853250339231720 vs pin 100871560902752353; `:927` vid 3850374787 vs 916304603 | **main** (`622982b1` / `1be19a55`) | See note A below. |
| `FactoryOos.SubwindowMetrics_SinglePass_BitIdentical` | `:1530` 346342504048605086 vs pin 14814588614960253351 | **main** | Same as above. |
| `FactoryOos.HoldoutEngineReuse_DigestUnchanged` | `:1622` 8553406461895070993 vs pin 5867665479471522971; `:1626` vid 3019293139 vs 3123399341 | **main** | Same as above. |

**Note A (the three `FactoryOos` reds):**

- The pins are byte-identical at `4e0aa98f`, `179af41c`, `4cd39925` and `dec03454`.
- The sprint side was green with these pins: factory 392/392 at p9-1k (`ad406718`), whose factory and engine code
  equals the sprint head; only research/store changed after it.
- Main rewrote CPCV aggregation in `fitness.cpp` (`622982b1`) and admission in `factory.cpp` (`1be19a55`) without
  re-pinning, and main's filter excluded `FactoryOos.*`.

Not rebuilt at `179af41c`: that needs a checkout switch and a near-full rebuild (not cheap; pool-25 only). Attribution
is by reading plus the owner's matching WIP. Fixing any of these means re-pinning or a code change, which is a ruling
(never re-pin to pass).

## Configure failure at `CMakeLists.txt:428`: pre-existing on main, not the merge, not the lease

- **What fails:** the `dev` preset (non-equity, `ATX_EQUITY_ONLY` unset). `main-configure-dev-repro.log` shows the
  first errors at `CMakeLists.txt:405/409/410/414`: `add_subdirectory` on `atx-vol`, `atx-kb`, `atx-agent-db`,
  `atx-options-engine`. None of these directories exists.
- **The `:428` errors are a consequence.** `include(cmake/atx-vol-install.cmake)` then calls `set_target_properties` /
  `install(TARGETS)` on the missing `atx-vol*` targets (lines 89-91, 117, 142).
- **Why the directories are missing:** all four were deleted by `e4bdcf54` ("clean up", 2026-09-19). They are not
  tracked at `179af41c`, `4cd39925` or `dec03454`, and they are absent in `C:/atx` too. They are not submodules
  (`.gitmodules` lists only databento-cpp).
- **The merge did not cause it.** The top-level `CMakeLists.txt` is unchanged base -> main -> merge; only
  `CMakePresets.json` differs (+7/-1, sprint side).
- **Why the lease hits it:** `scripts/lease-worktree.ps1:710-716` always configures `dev` / `dev-shared`. So every lease
  since 2026-09-19 fails this step, on any branch.
- **The equity presets are unaffected.** `ATX_EQUITY_ONLY=ON` (`CMakePresets.json:106`), and `equity-dev` configured
  fine (`main-configure-equity-dev.log`).
- **Related:** the `CLAUDE.md` text about `atx-vol` is stale for the same reason.
- **Fix options (owner / main-side, not done):**
  - guard the four `add_subdirectory` calls and the install include with `EXISTS`, or
  - drop them, or
  - have the lease configure an equity preset.

## For the owner to decide

1. **`origin/main` already points at `dec03454`.**
   - The reflog shows `refs/remotes/origin/main` "update by push" at 2026-10-03 23:02:04 -0400, about 1.5 min after
     local main moved; `git ls-remote origin refs/heads/main` returns `dec03454`. The ledger says "no push".
   - The recorded undo `git update-ref refs/heads/main 179af41c dec03454` would move local main only. The remote would
     need a force-push or a revert commit, which is the owner's call. I did neither.
2. **The 6 main-side factory reds:**
   - Your `C:/atx` WIP appears to address the two `DeflateSelection` ones.
   - The 3 `FactoryOos` pins and the NSGA gen-0 rule need a ruling: is `40357c1f`'s gen-0 N and `622982b1`'s
     re-scoring the intended contract (then re-pin with a reason) or a defect?
   - Undoing the merge would not clear them.
3. **The `dev`-preset configure defect** (above): fix it on main or point the lease at an equity preset.
4. **Hygiene left behind by the interrupted session:** git-ignored `__pycache__` dirs in pool-25, under `scripts/`,
   `scripts/tests/`, `scripts/tests/fixtures/`, `atx-engine/tools/`, `atx-engine/tests/fixtures/research_{fields,store}/`,
   `atx-impl/{tools,strategies}/` and `atx-db/src/atx_db/`. I left them alone (one is under `atx-db/`); they are
   harmless and can be deleted by name.

## Not run here

- Release (`BookNormalScore.*` known reds).
- pytest `atx-impl/tools` and `atx-impl/strategies`: no file of theirs is on both sides, and nothing in them is a seam.
- gtests for research store / catalog / admission: sprint-only code, and the CMake union is exact.
- Any build at `179af41c` or `4cd39925`.

## Housekeeping

- **Lease:** pool-25 is still held (run `p9-main-merge-20261003`, keeper pid 20308 alive). Not released.
- **Disk freed:** about 3.6 GiB. I deleted pool-25's `build-equity` object tree by name:
  - directories `atx-core`, `atx-engine`, `atx-impl`, `atx-tsdb`, `bin`, `lib`, `CMakeFiles`, `tests`
  - files `build.ninja`, `.ninja_deps`, `CMakeCache.txt`, `compile_commands.json`, and the cmake/cpack/ctest files.
- **Kept:**
  - every `main-*` / `mainv-*` log
  - the `mega-*` build logs and receipts
  - `.ninja_log`, `vcpkg-manifest-install.log`
  - the tracked `audits/` and `v8-interim3-pitch-render-run2/`.
- **Left in place:** `pool-25/build/`, the failed `dev` configure from the lease (evidence, small).
- **Other:** no process of mine is running. No trial-ledger access.
