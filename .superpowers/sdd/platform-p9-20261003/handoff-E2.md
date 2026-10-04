# Handoff: lane E2 (P9), fix round 1, stopped by the owner

- Pool: `C:/atx-wt/pool-17`. Branch: `feat/p9-e2-20261003`. Base: `1239a5ff`.
- Code head: `fe140597` (`wip(e2): fix round 1 ...`). This file is committed on top of it; see `git log -1`.
- Review being fixed: `.superpowers/sdd/platform-p9-20261003/task-E2-review.md` (`780a4934`): CHANGES_REQUIRED, R1 + R2,
  S1-S9. The PM also added W1-I1 and W1-I2 (a) (rulings in pool-2 `progress.md`, details in `wave1-whole-review.md`
  I-1 / I-2).
- About 85% of fix round 1 is done. Every required item is in the wip commit with tests. Still to do: the full-suite
  re-run under two seeds, the "Fix round 1" report section and the final commit.
- No process of this lane is running (checked: no python with pool-17 or e2f- in its command line).

## State per item (all committed in `fe140597`)
| item | state | where | test |
|---|---|---|---|
| R1 seal scan of the builds | DONE | `wave_stage_kinds.BUILDS` / `build_bases`; `wave_seal.wave_logs` adds `<out_dir>/builds/<what>-run<k>`; `run_build` refuses a name outside BUILDS; the verify plan line names the builds for v2 field / role waves only (v1 plan bytes unchanged) | `test_wave_kinds.py::test_build_logs_are_seal_scanned`: a field wave whose fake build log says 2024-06-28 gives verify exit 4, naming the log; a clean log passes |
| R2 raw rule flags | DONE | `wave_kinds.table_flags` / `raw_rule_flags` (the named rules' derived flags plus every table rule's flags: composition flags, NAV x-flag, param x-flags); preflight checks both `rule_cell.constants.flags` and the template's `change.flags`; `owned_flags` removed (superseded) | `test_rule_wave_refuses_raw_rule_flags` (3 params: the named rule's `--aim-leverage`, an unnamed `--vol-target`, `--theme-erc`; constants and template; `--cadence` still allowed); `test_rule_flags_from_the_tables` |
| W1-I1 path length | DONE | `cycle/phases.py` `CACHE_*`, `deepest_cache_path`, `cache_path_problem`, `library_ids`, `Cycle.cache_problem` / `cache_path_check` (called in `steps()` before u; exit 2); wave `wave_stage_preflight.cache_path_check` (exit 3, before register); facade re-exports. Layout taken from the C++ (see the comment block): Release identity `dslvm1_clang18.1_opt_md_ndebug_xs13.0.0` (40 chars with its `/`), `_audit` +6 under `--eval-mode audit-exact`, fp_ level always counted, `.partial` leaf. The exe prints no identity, so this is a documented bound. Applied only while the spec's IC exe is on disk (only the exe writes the cache) | `test_wave_driver.py::test_cache_path_bound_w1_i1`: under the v8 cache of pool-2 the path is 226 + len(id), matching the review's derivation, so ids <= 24 pass and <= 18 with audit; the cycle and the wave refuse; nothing is refused without the exe; CAND_ID_RE stays at 64 |
| W1-I2 (a) parent exe | DONE | `wave_stage_record.exe_problem`: for `ref == "none"` (a rule cell) it refuses when the receipts' exes differ, when the pins name two NAV exes, or when only one side records an exe; it passes only when neither does (synthetic). Both hashes are in the message | `test_rule_cell_parent_nav_from_another_exe_is_refused` (5 params, full fake rule wave to verify); `test_lock_exes_pins_and_verify_compares`: the old "rule cell: recorded only" assertion is replaced by the ruled refusal |
| S1 unwritable claims dir | DONE | `run_bounded_research.ClaimsDirError` (a PermissionError for the whole wait with no lock on disk); `reclaim` returns True only when it removed the lock | added to `test_claims_lock_failures_are_admission_waits` |
| S2 replay on interrupt | DONE | `Wave.side_by_side` replays its buffers in `finally` | none new (the existing judge-order test passes) |
| S3 schema type list / malformed keywords | DONE | `wave_kinds.schema_problems` | `test_schema_subset` |
| S4 placeholder regex | DONE | `PLACEHOLDER_RE = [A-Z][A-Z0-9_]*` | `test_schema_subset` |
| S5 normalised field.output | DONE | `wave_kinds._norm` (drops `.`, case-blind) | `test_v2_refusals` (+2 rows) |
| S6 build reuse | DONE (receipt part) | `run_build` reuses an existing output only when this wave's last `<what>` run completed it with this command's argv_sha256, else exit 3. Not done: the role manifest seal / end check (the role builder enforces it; its manifest layout is the builder's) | `test_field_wave_builds_then_registers_on_the_build` (+2 refusals); KindsFake receipts now carry argv_sha256 |
| S7 report plan changes | DONE | `task-E2-report.md` "How root verifies" item 5 now lists the sticky-pin plan change | - |
| S8 in_code copy | NOT DONE, by choice | moving it into `research_tree` is a cross-lane edit to a shared module (import cycle wave_manifest <-> wave_kinds). Report it as not fixed | - |
| S9 missing tests | DONE | leverage bounds, persistent PermissionError, build seal scan | `test_leverage_wave_refuses_a_value_outside_the_nav_table` and the tests above |

Cross-lane-ish test edit: `scripts/tests/test_research_spec.py` (E2-owned) gains the fixture `stand_in_ic`, which lifts
`CACHE_PATH_MAX` for the 3 add-alpha tests on `v8_root`. Their IC exe is a stand-in (`bin/ic.cmd`) that writes no cache,
and pytest tmp roots are deeper than any pool, so W1-I1 would refuse the committed v7.1 / v8 ids there. No assertion
was changed.

## Tests run (this round)
- `scripts/tests` full, PYTHONHASHSEED=0, before the `stand_in_ic` fixture: 4 failed / 429 passed / 4 skipped. The 4
  were the 3 v8_root add-alpha tests plus `test_the_whole_file_passes_with_a_generated_spec_present`, which re-runs them.
- After the fixture: `test_research_spec.py` 87 passed. `test_wave_kinds.py` 49 passed. The targeted test_wave_driver
  tests passed: cache_path_bound, parent_nav (x5), lock_exes, claims_lock, launch_waits, two_launches, adopt_never,
  judge_records, k_p9_10.
- NOT run after the fix: the full `scripts/tests` under seeds 0 and 1, and `atx-engine/tools/test_stage_chain.py`
  (separate invocation; its stage_chain is untouched this round).

## Next steps (in order)
1. Full suite, explicit paths, as SEPARATE invocations (conftest window trap), each with its own `--basetemp` in the
   scratchpad:
   `PYTHONHASHSEED=0 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider -rfEs scripts/tests`
   then `... atx-engine/tools/test_stage_chain.py`; repeat with `PYTHONHASHSEED=1`. About 15-22 min per seed. Expect
   0 failed. A red: fix it, never edit an expected value to fit.
2. Append "## Fix round 1" to `task-E2-report.md`: the table above, evidence (commands, exit codes, tails), decisions
   (below), the S6 role-seal and S8 reasons, the W1-I1 margin finding (below). Commit as `fix(e2): ...` (squash nothing),
   `git add -f` the report, trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
3. Delete own scratch by name in the session scratchpad: `e2f-pt-s0`, `e2f-pt-spec`, `e2fix-w`, `e2fix_try_verify.py`,
   `e2fix_pathlen.py`, `e2f-seed0.log`, plus the new run dirs. Delete `__pycache__` under `scripts/`, `scripts/cycle/`,
   `scripts/tests/`, `scripts/tests/fixtures/`, `atx-engine/tools/`, `atx-impl/tools/`.
4. Final message to the PM: status, head SHA, test summary.

## Decisions (for the report)
- W1-I1 is gated on the IC exe being on disk. Reason: only the exe writes the cache, so nothing is computed without it
  (refused before any compute holds). It also keeps hash-only plans and fake-tool fixtures as before. The cycle check
  needs the library on disk (its ids); a hash-only plan skips it.
- W1-I1 bound is conservative: the fp_ level is counted for every candidate (base candidates do not have it), and the
  Release identity is used for every build (the Debug identity adds no level).
- W1-I2 (a) is keyed on `ref == "none"` (no reference construction), which is broader than "rule cell" by name: a
  library cell without a ref phase has the same exposure.
- verify_plan text for v1 manifests is unchanged (plan identity). For rule waves the text still says "recorded", but
  the code now refuses: flag this to root as a known stale plan line, or change it with root's OK.

## Findings for the PM / root
- **W1-I1 margin is tight on committed specs** (scratch scan over every committed cell spec with an IC pass, Release
  bound): longest id `seasonality_same_month` (22 chars), worst spec `scripts/specs/v70-lo3.json`
  (`build-equity/mega-candidate-cache-v70-lo3`): 249 at `C:/atx-wt/pool-2`, **250 at `C:/atx-wt/pool-17`** (equal to
  the bound, so not refused), 239 at `C:/atx`. A pool path one character longer (e.g. `pool-100`), or any longer cache
  dir, refuses those committed specs at plan once the IC exe is built there. Root's plan-identity run in pool-2 stays
  within the bound. Note this in the report.
- The ruling's "AL / PRE ids <= 24 chars" matches exactly: under the v8 cache in pool-2 the bound admits 24 (18 with
  audit-exact).

## Open questions
1. The W1-I1 exe-on-disk gate: acceptable? The alternative (always check) refuses every fake-tool fixture with
   candidate ids in deep pytest roots.
2. Should verify_plan's NAV-exe text change for rule waves (changes `research_wave.py plan` bytes for v1 rule
   manifests)?
3. S6 role-manifest seal check: wanted in this lane, or leave it to the builder?

## Traps
- `rtk` rewrites grep/git output: use `rtk proxy <cmd>` or the Grep tool. Never run `scripts/tests` and any
  `atx-engine/tools` test in one pytest session.
- Failed-stage messages go to stderr; tests read them from `receipts/NN-<stage>.failed-1.json` ["error"].
- Blind rule: no 2020-2023 return / IC / NAV outputs; the seal tests use synthetic post-2024 text (ruling T2-SYN).
