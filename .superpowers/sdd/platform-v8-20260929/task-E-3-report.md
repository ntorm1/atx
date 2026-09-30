# Report: task E-3 (tiny_world fixture, end-to-end test, ctest registration)

Lane EV, pool-8, branch `feat/platform-v8-ev-20260929`. Status: DONE_WITH_CONCERNS (goldens are null until root
records them; the live test waits for contract K3 `--no-git` from lane A).

## What was built

| file | content |
|---|---|
| `scripts/tests/fixtures/tiny_world.py` | `build(root, seed=7, *, bin_dir=None, python=None) -> dict`, `simulate(seed) -> dict`, CLI `python tiny_world.py ROOT [--seed S] [--bin DIR]` |
| `scripts/specs/tiny.json` | the cycle spec TEMPLATE with the four input pins locked (library, recipe, role, fields manifest) |
| `scripts/tests/test_cycle_e2e.py` | 4 offline tests + 1 live test; `--record` mode |
| `scripts/tests/fixtures/tiny_world_goldens.json` | golden digests, all null (root records) |
| `atx-impl/tests/CMakeLists.txt` | `gtest_discover_tests` for the three strategy executables, label `atx_equity_strategy` |

`build()` writes under a new or empty ROOT (never overwrites) and returns
`{role_manifest_sha256, fields_manifest_sha256, library_sha256, recipe_sha256, registry_sha256, spec, seed}`:

- `role/` `atx.recent-research-role/v1`, exactly the keys `prepare_recent_research.create_role` publishes and every
  check `read_strategy_role`, the IC runner's `admit`, the fitter's `RoleManifest` and the target replay's
  `load_prices` apply: 656 weekday sessions ending 2022-12-30 x 64 names (ids 1001..1064), score_begin 384,
  score_start_ns = session 384, score_end_ns = last session + 1 day, member from session 63, top_n 64,
  `declared_output_bytes` = 656 x 64 x 26 + 656 x 8 + 64 x 8, plus a `synthetic` block naming the generator.
- `fields/` `atx.research-role-fields/v1` bound to the role (manifest, sessions, ids, member SHAs, shape), rows
  `shares_out`, `si_shares`, `tiny_signal`, all `point_in_time: true`, `non_pit_aspects: []` (IC runner `bind_fields`
  and NAV `load_fields`; shares_out and si_shares switch the NAV to the tiered financing matrix, so the primary
  scenario is `modeled-1bn-stale5-v1+swap-fin-v1` as in v7.1).
- `tiny_world_v1.json` (`atx.dsl-ic-library/v1`), `tiny_world_v1.recipe.json` (lineage: theme, tier, prior sign,
  origin), `registry.json` (`atx.alpha-registry/v1`, the A-1 schema; origins prior, prior, grid, mined so K5 readers
  see all three classes).

| member | theme = family | tier | DSL |
|---|---|---|---|
| planted_a | short_interest | B+ | `rank(decay_linear((-1 * (si_shares / shares_out)), 21))` (the v7.1 si_ratio form) |
| planted_b | earnings_momentum | B | `rank(tiny_signal)` |
| noise_c | value | B- | `rank(shares_out)` (a state independent of every return) |
| copy_b | earnings_momentum | C+ | `rank((2 * tiny_signal))` (planted_b's ranks, another spelling) |

World: `numpy.random.Generator(PCG64(seed))`; r_i(t) = beta_i m(t) + .02 (.05 / sqrt(21) (zA_i(t-1) + zB_i(t-1)) +
e_i(t)); zA, zB, n are AR(.995) states with unit variance. The planted coefficient is in noise-SD units, so the rank IC
at h 21 is .05 to first order (checked on 1,500 names: .041 and .045 Spearman, the Pearson-to-Spearman factor .955 and
the AR decay .95 included). Normals are Irwin-Hall sums of 12 uniforms added in a fixed order and only + - * / and a
correctly rounded sqrt are used, so the bytes do not depend on the CPU's libm or SIMD path. JSON is written as bytes
with LF.

Admission preview (scratch script, the fitter's own `Context`, `screen_v4` on emulated signals, seed 7): planted_a,
planted_b, noise_c admitted (270 live TRAIN days, tau .05 / .12 / .03); copy_b `reject_redundant` with planted_b,
rho 1.0. Realized runner-style rank IC21 means: planted_a +.029, planted_b +.011 (both positive, so the runner signs
agree with the prior and the gate passes).

`tiny.json` (template) runs fields (as built, pinned) -> u -> fit (`--orientation prior --screen v4-prior-v1
--composition ew-theme-v1`) -> card -> gate (`planted_a`, `planted_b` admitted with the prior sign, require all) -> w ->
nav (the v7.1 reference construction flags; `--max-bytes 268435456`). Runner caps 60 s / 512 MiB / 256 MiB free.
`build()` writes `ROOT/tiny.json` with scripts absolute in this checkout, exes under `bin_dir` (default
`ATX_EQUITY_BIN`, else `<repo>/build-equity/bin`), `python` = the calling interpreter, and refuses when the computed
pins differ from the template's locked pins (seed 7 only).

Golden digests (`test_cycle_e2e.digests`): `orientations_json_sha256` (file), `primary_daily_csv_sha256` (file),
`admission_decisions_sha256` = SHA-256 of admission.json's canonical JSON without `inputs`, plus `primary_scenario`.

ctest (after `atx_impl_test_target_environment(atx-impl-strategy-target-tests)`):

```cmake
foreach(_strategy_tests IN ITEMS
        atx-impl-strategy-tests atx-impl-strategy-ic-tests atx-impl-strategy-target-tests)
    gtest_discover_tests(${_strategy_tests} DISCOVERY_MODE PRE_TEST TEST_PREFIX "${_strategy_tests}."
        PROPERTIES LABELS atx_equity_strategy)
endforeach()
```

## How root verifies

```bash
PY="C:/Program Files/Python312/python.exe"
"$PY" -m pytest -q -p no:cacheprovider scripts/tests/test_cycle_e2e.py      # 3 passed, 2 skipped (offline, ~2 s)
# after lane A's --no-git (K3) is merged and the Debug exes are built:
ATX_EQUITY_BIN=C:/atx-wt/pool-2/build-equity/bin "$PY" scripts/tests/test_cycle_e2e.py --record   # writes the goldens
git add scripts/tests/fixtures/tiny_world_goldens.json && git commit -m "test(platform): tiny_world goldens"
ATX_EQUITY_BIN=C:/atx-wt/pool-2/build-equity/bin "$PY" -m pytest -q -p no:cacheprovider scripts/tests/test_cycle_e2e.py
```

The live test asserts: exit 0; copy_b `reject_redundant` with planted_b and both planted members admitted; the three
digests equal the goldens; first run under `ATX_E2E_MAX_SECONDS` (default 15); a second run prints `== <phase>: done`
for fields, u, fit, card, w, nav, executes none of them, creates no new entry in ROOT and leaves the digests unchanged.
The P-4 canary: record with the Debug exes, then run the test with `ATX_EQUITY_BIN` pointing at Release exes.

ctest: reconfigure the equity tree, build the targets `atx-impl-strategy-tests`, `atx-impl-strategy-ic-tests`,
`atx-impl-strategy-target-tests` (E-1 `scripts\research-build.ps1 -Preset equity`, or `build-equity\mega-build.ps1`
before E-1), then `ctest --test-dir build-equity -N -L atx_equity_strategy` (list) and `-L atx_equity_strategy` (run).

Identity: E-3 adds tests, a fixture and ctest registrations only. No production source or accepted output changes.

## Deviations from the brief

1. **656 dates, not 448** (score_begin 384 kept, 64 names kept). 448 - 384 leaves 64 scored sessions, 62 decisions.
   The fitter's `v4-prior-v1` rejects every member with fewer than 250 live TRAIN days and treats pairs with fewer than
   250 common days as uncorrelated, so with 448 dates nothing is admitted (fit exits 4, the cycle hard-stops before w)
   and the copy can never be rejected as redundant. 656 dates give 272 scored sessions and 270 decisions.
2. **Last session 2022-12-30** (the rule allows 2023-12-29 or earlier). The fixture then runs on both the pre-W0-1
   tools (TRAIN ends 2023-01-01) and the W0-1 tools (TRAIN ends 2024-01-01); the fixture needs no window constant.
   `test_tiny_world_inside_the_research_window` checks it against `research_window.py` once W0-1 is merged.
3. **admission.json golden is a decision-table digest**, not the file SHA. admission.json's `inputs` block pins the u
   pass `summary.json`, which holds `wall_seconds` and cache hit counts, so the file changes on every run; it also pins
   the fitter's own script SHA-256 (any edit of fit_composition_weights.py, e.g. lane C-1, would move it).
4. **ctest count.** `ctest -L atx_equity_strategy` lists every test of the three executables: by a TEST/TEST_F count,
   243 in `atx-impl/tests/strategy_*_test.cpp` (the review's figure) plus 53 atx-engine tests compiled into the same
   executables (strategy_data 2, execution_* 30, ic_screen 16, ic_research 5) = 296. Root confirms with `-N`.
5. Line 54 of `atx-impl/tests/CMakeLists.txt` (the atx-impl-tests registration) is unchanged: the new registrations
   carry a `TEST_PREFIX` so their names cannot collide with the same sources registered there.
6. The spec template carries the pins, and `build()` resolves machine paths into `ROOT/tiny.json`: research_cycle runs
   every script relative to `--root`, and the scripts live in the repository, not in ROOT.

## Cross-lane edits

None.

## Open risks

- K3 interface: the live test and `--record` pass `--no-git` to `research_cycle.py run` only; lane A must forward the
  root and the flag to `run_bounded_research.py` (today it derives the root from its own path and requires the output
  inside the repository). Both skip with a message until `research_cycle.py --help` lists `--no-git`.
- An unbuilt strategy executable appears in a bare `ctest` as `<target>_NOT_BUILT` (fails). Label runs are unaffected.
- `env_path_prepend` (vcpkg DLL dirs) is copied from v71.json; another machine needs its own.
- The C++ contracts were checked by reading the readers (strategy_data.cpp, strategy_ic_runner.cpp admit/bind_fields/
  library, strategy_target_replay.cpp admit_saved/load_prices, strategy_nav_replay.cpp load_fields); no executable was
  run. A refusal would show as a hard stop in the first live run.
- The realized planted IC21 (+.029, +.011) is sampling-noisy by design (64 names, one year); the gate relies on its
  sign for seed 7 only.
