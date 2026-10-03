# Lane T1 report

## Outcome
DONE_WITH_CONCERNS: all five deliverables are committed. The planted-IC "within one SE" check is reported, not
asserted, because at seed 7 it fails for planted_b by the reference's estimate. This needs a PM ruling (Deviation 1).
Everything else is as briefed, adjusted by rulings P1, P2, P11 and P13. (Superseded by the "Fix round 1" section at
the end: PM ruling T1-SE, 2-SE band asserted.)

## Branch / SHA
`feat/p9-t1-20261003`, pool-19. All work is committed and the tree is clean.

| task | commit | merge |
|---|---|---|
| 1 canary | `8bde1f28` feat(canary) | now (slot 2) |
| 2 CTest registration | `57aa5e4b` build(ctest) | now |
| 3 tie fixture | `37a01ced` test(eval) | now |
| 4 guards | `019239f6` test(guards) | now: merge up to here |
| 5 class-C deletion | `dee23d2a` chore(strategies) | only after root's slice-2 `--check` exits 0 (see below) |
| report | the commit after `dee23d2a` (this file, doc only) | with the deletion, or cherry-pick |

A reorder that would put the report before the deletion (`git reset --hard`) was refused by the permission system,
so the report sits on top. Root merges `019239f6` for tasks 1-4. The head (deletion plus this report) is merged after
the `--check`.

## Frozen base / lease
base_sha=d7c1c520c3caa162ef453349b256669ce8de3c80; worktree=C:\atx-wt\pool-19; lease_name=pool-19;
lease_run_id=p9-t1-20261003; heartbeat_id=p9-t1-hb; keeper_pid=13684;
keeper_process_started_utc=2026-10-03T10:09:34.5037201Z

## Acquisition receipt
Root leased the pool before dispatch, and the lane did not re-acquire it (as dispatched). These are the `.atx-lease`
fields, read verbatim: version=3, run_id=p9-t1-20261003, agent=p9-t1, branch=feat/p9-t1-20261003, base_ref=d7c1c520,
acquired_utc=2026-10-03T10:09:35.5289337Z, owner_kind=heartbeat, keeper_ready_utc=2026-10-03T10:09:35.2535852Z.

## Files changed
- Task 1:
  - `scripts/tests/test_cycle_e2e.py` (rewritten live half, plus 7 new offline tests)
  - new `scripts/tests/fixtures/tiny_world_ic.py` (numpy reference of the runner's rank-IC estimate, and the
    planted value)
  - `scripts/tests/fixtures/tiny_world.py` (seal block, `build_type`)
  - `scripts/tests/fixtures/tiny_world_goldens.json` (schema v2, null per build type)
  - `scripts/specs/tiny.json` (fields pin relocked)
  - `scripts/research-build.ps1` (`-Canary`, `-CanaryRecord`, `-Python`)
- Task 2: `atx-engine/tests/CMakeLists.txt` and `atx-impl/tests/CMakeLists.txt`: one appended deferred registration
  block each, plus one comment line each.
- Task 3: new `atx-engine/tests/fixtures/eval_tie/`, containing `.gitattributes` (`* -text`),
  `generate_eval_tie.py`, `test_eval_tie_fixture.py`, `series.f64`, `ledger_records.json`, `cbb_starts.u16` and
  `expected.json`.
- Task 4: new `scripts/tests/test_no_versioned_scripts.py` and new `scripts/tests/test_no_python_mirror.py`.
- Task 5:
  - deleted the 11 class-C files from `atx-impl/strategies/`: `generate_fund_ic_v4`, `v42`, `v5`, `v6`, `v61`,
    `v70`, `v71`, `generate_price_volume_ic96_v2`, `generate_pv_fields_ic121_v3`, `generate_slow_ic_48` and
    `check_fund_ic_v6`. That is 5,069 lines, matching the audit.
  - deleted their 9 tests: `test_generate_fund_ic_v4`, `v42`, `v5`, `v6`, `v61`, `v70`, `v71`,
    `test_generate_pv_fields_ic121_v3` and `test_check_fund_ic_v6_ops`.
  - in the same commit, removed their 19 rows from `test_no_versioned_scripts.py`.

### Interfaces as coded

**Canary.** The live test is `test_cycle_e2e_goldens_ic_reference_redundant_copy_and_idempotent_rerun`.

Environment variables:
- `ATX_EQUITY_BIN` (directory of both exes).
- `ATX_EQUITY_BUILD_TYPE` = `Debug` (default) | `Release`.
- `ATX_CANARY_REQUIRED=1`: a missing prerequisite fails instead of skipping.
- `ATX_E2E_MAX_SECONDS` (default 15).

The live test asserts:
- exit 0 of `research_cycle.py run ROOT/tiny.json --root ROOT --no-git`;
- copy_b `reject_redundant` against planted_b, and planted_a / planted_b admitted;
- every member's h-21 rank-IC estimate in the u pass `orientations.json` equals `tiny_world_ic.research_ic`:
  - mean within 1e-12 absolute;
  - HAC SE within 1e-9 relative;
  - `required_hac_lag`, `valid_dates` and `calendar_dates` exact;
  - reason for the tolerances: the reference's per-row correlation uses numpy reductions, and the executable uses its
    own loops;
- the build type's goldens;
- the time budget;
- the idempotent rerun.

It prints the planted block. The goldens (schema v2) are:
`builds.<Debug|Release>.outputs = {u_orientations_json_sha256, fit_admission_decisions_sha256, fit_weights_sha256,
w_combined_f64_sha256, nav_primary_daily_csv_sha256}`, plus `primary_scenario`, `planted` and `recorded`.
`fields_manifest_sha256` names the world the goldens belong to. An offline test fails if it differs from
`tiny.json`.

Outputs are found by payload name in any attempt layout: `<out>/`, `<out>-<k>/` and `<out>/attempt-<k>/` (E1's
K-P9-10 sub-dirs). Pins are payload SHAs, never run-dir paths (preflight row 2).

`python scripts/tests/test_cycle_e2e.py --record --bin DIR --build-type T` checks the IC reference first. It then
writes `builds[T]` and leaves the other build type's entry untouched.

**`research-build.ps1 -Canary`.**
- It refuses at start unless the tag builds both `atx-equity-strategy-ic` and `atx-equity-strategy-targets`.
- After a successful build it runs pytest on `test_cycle_e2e.py` with `ATX_EQUITY_BIN=<BuildDir>\bin`,
  `ATX_EQUITY_BUILD_TYPE` (equity-dev Debug, equity-rel Release) and `ATX_CANARY_REQUIRED=1`.
- Output: `build-equity\mega-<Tag>-canary.log`.
- The receipt gains `Canary {Mode, BuildType, Bin, ExitCode, Log, WallSeconds}`, and the canary's exit code becomes
  the script's.
- `-CanaryRecord` runs `--record` instead.
- Without either flag, behaviour, DryRun output and the receipt are unchanged (no `Canary` key).

**Tiny world.**
- The fields manifest gains `seal: {exclusive_end: research_window.current()["SEAL_DATE"] (2024-01-01), rule}`
  (ruling P13). The value is read from the JSON, so a test harness's `bind` cannot change it.
- `build(..., build_type="Release")` drops the vcpkg `debug/bin` entry from `env_path_prepend`, which equals
  `research_cycle.BUILDS["equity-rel"]["path"]` (tested).
- Only the fields manifest moved: `dbcacd2e...` became `395f82f7b34dd96e50403ff8b47faa58a3d7a94af433ddaaf906f596d039a1ac`.
  Role, library and recipe pins are unchanged.

**CTest.**
- Each test directory appends `atx_*_register_research_tests()`, run by `cmake_language(DEFER CALL ...)` at the end
  of that directory.
- The call is guarded by `if(TARGET)` and by `CTEST_DISCOVERED_TEST_COUNTER`, so nothing registers twice.
- It runs `gtest_discover_tests(<t> DISCOVERY_MODE PRE_TEST TEST_PREFIX "<t>." PROPERTIES LABELS atx_research)`.
- Engine directory: `atx-engine-research-fields-tests`, `atx-engine-research-admission-tests` (B1's name, if it
  exists).
- impl directory: `atx-impl-strategy-mine-tests`, plus B1's name as a fallback if B1 defines it outside the engine
  tests directory.
- The three strategy exes keep `atx_equity_strategy`, unchanged. There is one label per target: a list value does
  not pass through `PROPERTIES` intact. Use `ctest -L "atx_research|atx_equity_strategy"` for both sets.

**Tie fixture (P11: the fixture plus a Python-side verifier only; no C++ gtest this wave).** Contents:
- a 500 x 12 synthetic panel;
- 12 synthetic construction lines (window `eval-tie-v1`) for house_v1;
- the CBB starts (4999 x 24, `default_rng(20260929)`);
- the ONC k-means++ pick trace (920 picks).

Values are produced by calling today's `backtest_integrity` / `nav_summ` / `dsr_total` as libraries:
- `moments`, `net_moments`, `psr` (PSR and MinTRL at SR* 0 and .5), `norm_ppf`, `dsr_cross`, `dsr_lo`;
- `dsr_house_v1`: `v8` is house_v1, `tot` the total-count row;
- `pbo` (exhaustive, 12,870 splits) and `pbo_margins`;
- `onc`, `onc_replay`, `dsr_onc`;
- `paired`: dSR, rho, Memmel SE, t, CBB CI, LW CI and p-values (block 21, seed 20260929, 4,999 draws).

`tie_rules` gives B2 a tolerance and a reason per value. The rule is 0 for integer results while the recorded
margins stay large; the PBO margins are 1.2e-6 (IS) and 1.7e-7 (OOS). Otherwise it is 1e-12: pairwise vs sequential
sums, and bisection vs Acklam+Halley `norm_ppf`. `generate_eval_tie.py --check` (and the pytest) recomputes every
value from the committed inputs:
- exact on the recorded numpy 1.26.4, else 1e-12 relative;
- checks the starts are what `paired_stats` draws;
- replays ONC from the trace.

**Guards.**
- `test_no_versioned_scripts.py`:
  - covers names with a `_v<digits>` token in a `.py` file and `generate_*_v*` files of any extension;
  - scans tracked and untracked-not-ignored files;
  - excludes `atx-db/`, `archive/`, `.superpowers/`, `docs/`, `deps/` and `build*/`, with the reason stated in the
    file;
  - the allowlist is frozen at d7c1c520 at 30 rows, each with its retiring lane;
  - a stale row fails, and the list cannot grow.
- `test_no_python_mirror.py`:
  - AST scan of `scripts`, `atx-engine/tools`, `atx-impl/tools`, `atx-impl/strategies` and `python` (tests and
    fixtures excluded);
  - looks for definitions of the P9 list's rule functions;
  - 16 frozen rows: module, symbols, rule, deciding C++ (checked to exist, unless "new in <lane>") and retiring lane;
  - a stale row or symbol fails, and the list cannot grow;
  - `BINDINGS` (atxpy's `_core` wrapper) are checked to still call the core;
  - root sets `P9_FREEZE = True` at the freeze, which makes every row retired by a P9 lane an error;
  - it prints the G-P5 count, today `37 mirrored rule function(s) in 14 row(s) left for P9 lanes`.

## Evidence
All commands ran in `C:/atx-wt/pool-19`, with `PY="C:/Program Files/Python312/python.exe"`.

1. New and changed tests under both hash seeds, at the deletion head `dee23d2a`:
   `PYTHONHASHSEED=0` and `=1`: `$PY -m pytest -q -p no:cacheprovider scripts/tests/test_cycle_e2e.py
   scripts/tests/test_no_versioned_scripts.py scripts/tests/test_no_python_mirror.py
   atx-engine/tests/fixtures/eval_tie/test_eval_tie_fixture.py`. Both seeds gave exit_code=0:
   ```
   G-P5: 37 mirrored rule function(s) in 14 row(s) left for P9 lanes
   29 passed, 1 skipped in 4.04s        (seed 0; the skip is the live canary without ATX_EQUITY_BIN)
   29 passed, 1 skipped in 3.75s        (seed 1)
   ```
2. `$PY atx-engine/tests/fixtures/eval_tie/generate_eval_tie.py --check` gave exit_code=0:
   ```
   eval_tie: tied
   ```
3. The other users of the tiny_world fixture, after the seal block: `$PY -m pytest -q -p no:cacheprovider
   atx-impl/tools/test_era_data_audit.py atx-impl/tools/test_nav_summ_pool.py atx-impl/tools/test_book_diagnostics.py
   scripts/tests/test_research_cycle_roles.py` gave exit_code=0:
   ```
   73 passed in 22.83s
   ```
4. Full suites before the deletion, at `019239f6`: `$PY -m pytest -q -p no:cacheprovider -x scripts/tests
   atx-impl/strategies` gave exit_code=0:
   ```
   502 passed, 4 skipped in 369.46s (0:06:09)
   ```
5. The same after the deletion, at `dee23d2a`: `$PY -m pytest -q -p no:cacheprovider scripts/tests
   atx-impl/strategies` gave exit_code=0. The 154 fewer tests are the deleted class-C tests.
   ```
   348 passed, 4 skipped in 325.92s (0:05:25)
   ```
6. The slice-2 spec check on the lane tree after the deletion, with the committed fixture plan rather than root's
   live plan: `cd atx-impl/strategies && $PY generate_from_spec.py --spec specs/library-v71.json --check` gave
   exit_code=0:
   ```
   fund_industry_ic_v71.json 787c802ed1cdf7cfc507500b014525c3d4dff148f44e77ebd8368c0a686c2259 39317 bytes
   fund_industry_ic_v71.recipe.v2.json 69e95298470d1e4fc38b7b4e6d35809740b5a1479f6c5be1c34a8daac3017107 25389 bytes
   frozen: 19 artefacts verified; plan (alphas/fixtures/v71_plan_k1.json): 48 K1 rows within budget
   ```
7. DryRun only; nothing was built. `powershell -NoProfile -File scripts\research-build.ps1 -Tag t1-dry -Targets
   "atx-equity-strategy-ic,atx-equity-strategy-targets" -Preset equity-rel -Canary -DryRun` gave exit_code=0 and
   printed the unchanged plan block, then:
   ```
   Canary : ATX_EQUITY_BIN=C:\atx-wt\pool-19\build-equity-rel\bin ATX_EQUITY_BUILD_TYPE=Release ATX_CANARY_REQUIRED=1 &
            'C:\Program Files\Python312\python.exe' -m pytest -q -s -p no:cacheprovider scripts\tests\test_cycle_e2e.py
            *> C:\atx-wt\pool-19\build-equity\mega-t1-dry-canary.log
   ```
   With `-Targets "atx-equity-strategy-ic"` only, it refuses with exit 1: `research-build: -Canary needs the tag to
   build atx-equity-strategy-ic and atx-equity-strategy-targets (missing: atx-equity-strategy-targets)`. The script
   also parses cleanly with `[System.Management.Automation.Language.Parser]::ParseFile`.
8. The reference's prediction of the u pass at seed 7, h 21, offline from `tiny_world_ic.research_ic` and
   `planted_rank_ic`:

   | member | mean IC | HAC SE | t | planted | z | within 1 SE |
   |---|---|---|---|---|---|---|
   | planted_a | .02864 | .01884 | 1.52 | .04407 | -0.82 | yes |
   | planted_b | .01069 | .02998 | 0.36 | .04491 | -1.14 | no |
   | noise_c | .03543 | .01590 | 2.23 | 0 | | |
   | copy_b | identical to planted_b | | | | | |

   Every member has 250 valid dates and HAC lag 42. The smallest relative rank gap on any score row is 4.4e-7, so
   the executable's arithmetic cannot reorder ranks.

## How root verifies

**Canary goldens, Debug then Release (G-P8).** The canary runs after the first build in which every wave-1 merge
that moves u, fit, w or NAV bytes has landed. C1 moves NAV bytes at slot 8, so record after slot 8, or re-record then.
1. Debug: `powershell -File scripts\research-build.ps1 -Tag p9-1x -Targets
   "atx-equity-strategy-ic,atx-equity-strategy-targets,atx-engine-research-fields-tests,atx-impl-strategy-mine-tests"
   -CanaryRecord`.
   - This builds, checks the IC reference, and writes `builds.Debug` in
     `scripts/tests/fixtures/tiny_world_goldens.json`.
   - Commit that file.
   - Then verify green on the committed goldens: either a second tag with `-Canary`, or directly
     `ATX_EQUITY_BIN=C:/atx-wt/pool-2/build-equity/bin ATX_EQUITY_BUILD_TYPE=Debug ATX_CANARY_REQUIRED=1
     $PY -m pytest -q -s -p no:cacheprovider scripts/tests/test_cycle_e2e.py`. Expect `12 passed`, with a printed
     planted line.
2. Release: the same with `-Preset equity-rel -Targets "atx-equity-strategy-ic,atx-equity-strategy-targets"
   -CanaryRecord` (Bin `build-equity-rel\bin`, Release DLL path), then `-Canary`.
3. Compare `builds.Debug` with `builds.Release`. Under S1's claim, `u_orientations_json_sha256`,
   `fit_admission_decisions_sha256`, `fit_weights_sha256` and `w_combined_f64_sha256` should be equal (G-P3 evidence
   for the IC exe). `nav_primary_daily_csv_sha256` may differ until C1's `sqrt` probe decides (DEC-11). Log both.
4. Every later tag: `-Canary` must be green. A ruled re-pin is `-CanaryRecord` plus a ruling line.

**CTest count (G-P4).**
- Build `atx-engine-research-fields-tests` and `atx-impl-strategy-mine-tests` (equity-dev).
- Then run `powershell scripts\atx-build.ps1 -Ctest -Preset equity-dev -N -L atx_research`.
- Expected at the T1 head: Total Tests 75.
  - 31 from `atx-engine-research-fields-tests`: Clock 7, Writer 8, VolumeMean 5, FinraAsof 5, Fixture 3, Cli 3.
  - 44 from `atx-impl-strategy-mine-tests`: `strategy_mine_test.cpp` 29 + `factory_signal_fitness_test.cpp` 15.
- After A2 and B1 merge, add their new cases: A2's go in the fields target; B1's are counted if its target is named
  `atx-engine-research-admission-tests`, otherwise add the name to the deferred list (one word).
- An unbuilt target shows as `<target>_NOT_BUILT` without the label and is not counted.
- `ctest -N -L atx_equity_strategy` should equal its pre-merge count; that label is untouched.

**Tie (G-P7 first half).**
- Run `$PY -m pytest -q -p no:cacheprovider atx-engine/tests/fixtures/eval_tie/test_eval_tie_fixture.py`
  (7 passed) and `$PY atx-engine/tests/fixtures/eval_tie/generate_eval_tie.py --check` (exit 0).
- There is no tie gtest this wave (ruling P11). B2's EvalVerb gtest reads `expected.json` and the three input files.

**Guards.** Run `scripts/tests` under two seeds, as R0-2 already does. Both guards run there.

**Class-C deletion `dee23d2a`: what root's slice-2 `--check` must confirm before merging it.**
1. In `atx-impl/strategies` on root's tree, `$PY generate_from_spec.py --spec specs/library-v71.json --check
   --plan-json <the IC exe's live --plan-only output for fund_industry_ic_v71.json>` exits 0. This is the identity run
   the spec note names; the lane ran it only with the committed fixture plan.
2. It prints `fund_industry_ic_v71.json 787c802ed1cdf7cfc507500b014525c3d4dff148f44e77ebd8368c0a686c2259 39317
   bytes`, equal to the committed file.
3. It prints `fund_industry_ic_v71.recipe.v2.json 69e95298470d1e4fc38b7b4e6d35809740b5a1479f6c5be1c34a8daac3017107`.
4. It prints `frozen: 19 artefacts verified`: every library and recipe the 11 generators wrote, by SHA-256. The 19:
   `fund_industry_ic_v4`, `v42`, `v5`, `v6`, `v61`, `v70` (each `.json` and `.recipe.json`), `v71.recipe.json`,
   `price_volume_ic96_v2`, `pv_fields_ic121_v3` and `slow_price_volume_ic48_v1` (each `.json` and `.recipe.json`).
5. The plan's K1 rows are within budget on the live plan.
6. After the merge, `scripts/tests` and `atx-impl/strategies` pytest are green (lane head: 348 passed, 4 skipped), and
   nothing outside the deleted files imports them (`git grep` shows only docstrings, frozen recipe `generator`
   fields, the spec's documentation-only `replaces` list, and archived `.superpowers` studies).

## Deviations from brief
1. **The planted-IC check is reported, not asserted** (brief T1 (1): "checks the planted members' mean IC and HAC t
   within one SE of the planted value").
   - The canary computes the planted members' z = (mean - planted) / SE and "within one SE", prints them, and stores
     them in the goldens.
   - It asserts a stronger, seed-independent property instead: the executable's mean IC, HAC SE, lag and dates equal
   an independent numpy reference of the runner's recipe (`fixtures/tiny_world_ic.py`, coded from
   `ic_screen.cpp` / `hac.hpp`).
   - Reasons:
     - At seed 7 the reference puts planted_b at z = -1.14 (planted .0449, estimate .0107, SE .0300). A literal
       assertion would make the canary red on a correct executable.
     - Across seeds 1-28 (synthetic, offline, the same estimator, planted value taken as .045 for both members), only
       6 pass the one-SE band for both planted members. The runner's
       HAC SE at lag 42 on 250 overlapping dates under-covers the realised dispersion by about 1.5x, so a one-SE band
       holds about half the time per member, whatever the world size.
     - Choosing a seed that passes would be post hoc.
     - The fixture world is shared by `atx-impl/tools/test_{book_diagnostics,nav_summ_pool,era_data_audit}.py`, so it
       was not changed.
   - Options for the PM:
     - (a) keep it as built (reported);
     - (b) pre-register a 2-SE band now (seed 7 passes: z -0.82 and -1.14);
     - (c) rule the check out of the canary.
   - Cost if wrong: DS section 6's "within one SE" stays unchecked as an assertion.
2. **No C++ tie gtest** (brief T1 (3)): per ruling P11, which overrides the brief. The fixture also carries the draws
   (CBB starts, ONC picks) so B2 needs no PCG64 port.
3. **The mirror guard's expiry is the retiring lane's deletion commit (stale rows fail) plus a `P9_FREEZE` switch**,
   not "the row must be gone once the replacing C++ exists" (preflight section 5 row 1). Plan section 0.6 keeps a
   Python copy one slice after root's identity run, and several replacements exist at the base already
   (`eval/deflated_sharpe.hpp`, `strategy_ic_composition.cpp`), so an existence trigger would fail immediately or at
   B2 / D2's merge.
4. **One label per target** (`atx_research` for the research exes; the strategy exes keep `atx_equity_strategy`).
   `gtest_discover_tests` does not carry a list-valued `LABELS` through `PROPERTIES` safely, and CMake was not run to
   prove an escape. `-L` is a regex, so `ctest -L "atx_research|atx_equity_strategy"` covers both.
5. **Goldens reset to null**, with the v1 values kept under `superseded_v1`. The P13 seal block moves the fields
   manifest SHA, and `orientations.json` embeds it, so the v1 goldens cannot hold. The brief says root records the
   goldens; no expected hash was edited to make a test pass.

## Cross-lane edits
- `scripts/specs/tiny.json`: `fields.manifest_sha256` relocked. No lane owns this file in wave 1; the change is a
  consequence of P13, done by the procedure in the fixture's own docstring.
- `atx-engine/tests/CMakeLists.txt` and `atx-impl/tests/CMakeLists.txt` are T1's per P2. The change is one comment
  line each, next to the fields-tests and mine-tests blocks, plus the appended block. A2 adds source lines to the
  fields target (lines 368-374), with no overlapping hunk.
- No other lane's file was touched.

## Open risks
- E1 (slot 1) changes run dirs and phase logging. The canary finds payloads by name in `<out>-<k>/` and
  `<out>/attempt-<k>/`. The idempotent-rerun check still reads `== <phase>: done` lines, so re-run
  `test_cycle_e2e.py` on E1's merged tree (preflight row 2).
- The IC reference tie is coded from the C++ source, not run against an executable, so root's first Debug run is the
  first evidence. A failure prints each member's executable value against the reference value, and `--record`
  refuses to record. If the tie fails, either the reference or the executable is wrong; the tolerance is not to be
  loosened.
- A ruled NAV re-pin after C1 should include `nav_primary_daily_csv_sha256` in C1's substitution list (preflight
  row 14).
- After the deletion, `scripts/specs/v61.json`, `v61-ops.json`, `v70.json` and `v71.json` still name
  `atx-impl/strategies/check_fund_ic_v6.py` as `static_check.script`. A replay of those historical cycles would fail
  at the check phase; tests compare argv strings only and stay green. The audit rules the file deleted. A spec edit
  (dropping `static_check` from historical specs) is not T1's and would move nothing else.
- The mirror guard knows rules only by function name. Wave-1 Python copies created by other lanes under new names
  (A1's reuse / seal rule, D1's plugin list; preflight section 5 rows 5-6) are not detected. Those lanes or the wave-2
  briefs should add rows; the frozen-size check makes that visible.
- `horizon_stats.py` has no owner in plan section 2.2. The row names D2 (the fitter imports it).
  `mega_report/data.py` (`memmel_se`) and `atxpy/pbo.py` (`cscv_pbo`) are mirrors outside the P9 list, recorded as
  P10.

## Ledger candidates
- tiny_world seed 7, h 21: the runner-recipe reference gives planted_a .0286 (SE .0188) and planted_b .0107
  (SE .0300) against a planted rank IC of about .044-.045. The runner's lag-42 HAC SE on 250 overlapping dates
  under-covers the realised dispersion by about 1.5x (6 of 28 seeds pass a one-SE band for both planted members).
- `gtest_discover_tests(... PROPERTIES LABELS a;b)` cannot carry two labels safely; register one label per target
  and select sets with the `ctest -L` regex.
- `generate_from_spec.py --spec specs/library-v71.json --check` verifies 19 frozen artefacts and 48 K1 rows with the
  committed fixture plan; the 11 class-C generators were 5,069 lines plus 2,752 test lines.

## Fix round 1 (review BLOCK at 3fbc94e0; PM ruling T1-SE)

### Outcome
DONE. The major finding is fixed: planted recovery is now asserted in a pre-registered 2-SE band. Deviation 1 above
is resolved by ruling T1-SE. Minor findings 2, 3 and 6 are fixed in code; findings 4, 5 and 7 are answered below.

### Commits (on top of `3fbc94e0`)
| commit | content | merge |
|---|---|---|
| `dd5c4f5e` fix(canary) | findings 1, 3, 6 and the mirror half of 2 | with tasks 1-4. It touches no file of the deletion commit: `git merge-tree --write-tree --merge-base=3fbc94e0 019239f6 dd5c4f5e` exits 0, so it cherry-picks cleanly onto `019239f6` |
| `10493041` fix(guards) | finding 2, versioned half | with the class-C deletion `dee23d2a` (it constrains the post-deletion allowlist) |
| the next commit | this section | doc only |

### Changes
**Finding 1 (major), `scripts/tests/test_cycle_e2e.py` and `fixtures/tiny_world_ic.py`.**
- New constant `PLANTED_BAND_SE = 2.0`, cited to PM ruling T1-SE.
- `planted_report(mean, se, planted, band_se)` returns `band_se` and `within_band` (`|z| <= band_se`). The one-SE
  flag is gone.
- `_live_cycle` asserts `planted_problems(planted_block(...)) == []` for planted_a and planted_b, after the IC tie and
  before the goldens. noise_c is not in the band: its planted value is 0 and it sits at z +2.23 at seed 7.
- `record()` refuses to write when a planted member is outside the band.
- New offline test `test_reference_planted_members_sit_inside_the_band_at_seed_7`:
  - reference z = -0.82 (planted_a) and -1.14 (planted_b), each within 0.005 and both inside the band;
  - noise_c at z +2.23, outside the band;
  - a moved estimate (z -2.5) is refused.
- The module docstring cites T1-SE.

**Finding 3, `record()`.**
- `record()` refuses before any run when `builds[<type>]` is already recorded, unless `--repin` is passed (a ruled
  re-pin, plan section 0.6). It then prints `re-pin <type> <key>: old -> new` for every digest.
- It applies the live test's admission facts through a shared `admission_problems()`: copy_b redundant with
  planted_b, planted_a and planted_b admitted. Before, only copy_b was checked.
- New tests:
  - `test_record_refuses_to_overwrite_a_recorded_entry_without_repin`: the goldens file is unchanged after the
    refusal.
  - `test_admission_problems_name_every_broken_fact`.
- `research-build.ps1 -CanaryRecord` therefore fails on an already recorded build type. A ruled re-pin runs
  `test_cycle_e2e.py --record --repin --bin DIR --build-type T` directly.

**Finding 2.**
- Versioned guard (`10493041`): the size cap is replaced by `set(ALLOWLIST) <= FROZEN_ROWS`, the 30 base rows.
- Mirror guard (`dd5c4f5e`): the row cap is replaced by live (module, symbol) pairs being a subset of
  `FROZEN_PAIRS`, the 39 base pairs (verified equal to the base allowlist).
- `RULE_SYMBOLS` now derives from `FROZEN_PAIRS`, so retiring a row never weakens the scan for that name.
- A retiring lane deletes rows and changes nothing else. A ruled new row (A1/A2's seal rule) is added to
  `FROZEN_PAIRS` by root.

**Finding 6, eval_tie.**
- `expected.json` gains `host` (numpy 1.26.4, openblas64 0.3.23.dev, AMD64, the CPU string).
- `--check` compares exactly only on an equal host identity, and at 1e-12 relative otherwise.
- `test_regenerating_writes_the_committed_bytes` skips on another host.
- Values and input SHA-256s are unchanged (the diff is +6 lines of host block).

**Finding 4 (historical v61/v70/v71 specs name the deleted `check_fund_ic_v6.py`).** No T1 file edit, as the
review asks. Proposed PM ruling line: "historical v61/v70/v71 specs are not replayable after the audit section 4
deletion; their libraries and recipes stay frozen, sha-pinned data -- cost if wrong: a replay needs the file from git
history".

**Finding 5 (handoff).** The eval_tie verifier imports `backtest_integrity`, `nav_summ` and `dsr_total`, which B2
deletes in wave 2, and no routine suite runs it.
- B2 retires or converts `test_eval_tie_fixture.py` and the `--check` path of `generate_eval_tie.py` in its deletion
  commit.
- The data files and `expected.json` stay as the inputs of B2's EvalVerb gtest.
- Until then root runs `$PY -m pytest -q -p no:cacheprovider atx-engine/tests/fixtures/eval_tie/test_eval_tie_fixture.py`
  explicitly each wave.

**Finding 7 (G-P4 scope).** These are not labelled because they are bounded duplicates whose sources CTest already
registers through their owning group targets or the labelled strategy exes:
- `atx-engine-ic-screen-tests` ("Do not duplicate CTest registrations", engine tests list);
- `atx-impl-ic-screen-tests`;
- `atx-engine-w1-eval-tests` and the other `atx-engine-w1-*-tests`;
- `ic_screen_test` / `ic_research_test` run in `atx-impl-strategy-ic-tests` under `atx_equity_strategy`.

Like the `atx_equity_strategy` precedent, an unfiltered `ctest` on a tree where `atx-engine-research-fields-tests` or
`atx-impl-strategy-mine-tests` is unbuilt lists `<target>_NOT_BUILT` placeholders, and those fail if run. Label-filtered
runs are unaffected. The admission target is confirmed by the review as `atx-engine-research-admission-tests` in
`atx-engine/tests`, which the engine-side deferred call registers.

### Evidence
1. Covering tests under both hash seeds, at `10493041`: `PYTHONHASHSEED=0` and `=1`:
   `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_cycle_e2e.py
   scripts/tests/test_no_versioned_scripts.py scripts/tests/test_no_python_mirror.py
   atx-engine/tests/fixtures/eval_tie/test_eval_tie_fixture.py`. Both seeds gave exit_code=0:
   ```
   G-P5: 37 mirrored rule function(s) in 14 row(s) left for P9 lanes
   32 passed, 1 skipped in 3.74s        (seed 0; the skip is the live canary without ATX_EQUITY_BIN)
   32 passed, 1 skipped in 3.21s        (seed 1)
   ```
2. `"C:/Program Files/Python312/python.exe" atx-engine/tests/fixtures/eval_tie/generate_eval_tie.py --check` gave
   exit_code=0:
   ```
   eval_tie: tied
   ```

### How root verifies (changes to the steps above)
The live canary now also fails when planted_a or planted_b lies outside 2 SE. The first `-CanaryRecord` per build type
refuses on a band miss, so a recorded golden always carries a recovered planted signal. Later changes need
`--record --repin` and a ruling.
