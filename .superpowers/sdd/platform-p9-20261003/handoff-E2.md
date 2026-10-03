# Handoff: lane E2 (P9, research cycle / wave runner, Python), stopped by the owner

- Pool: `C:/atx-wt/pool-17` (reused). Branch: `feat/p9-e2-20261003`. Base: `1239a5ff` (D1 merge, P9 integration head).
- Head before this file: `fa7d5805` (`wip(E2): ...`). This file is committed on top of it; see `git log -1`.
- Contract: `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/wave2-lane-dispatch.md` (LANE = E2), brief
  `briefs/brief-E2.md`, carry `wave2-carry.md` section E2.
- Status: about 85% done. Every brief deliverable and required carry item is committed. Not done: three carried E1
  minors, the P9 PM7-35 retirement (deferred by design), the full-suite re-run after the last four commits, and the
  task-E2 report.
- No process of this lane is left running (checked: no python.exe with pool-17 in its command line).

## Commits (base..head, oldest first)
| SHA | What |
|---|---|
| 98ffcf75 | M1a-RED: mine field-rule tests read the registry of record at the lock |
| 7e4573d5 | E1-N3: an inherited output is judged by the ancestor that made it (whole template chain) |
| 4d6699eb | move-only split of `research_cycle.py` into `scripts/cycle/{spec,resolver,phases,gate,compare,lock,cli}.py` |
| 4f770ec6 | `scripts/research_common.py`: one copy of the shared helpers; run-dir naming defined once |
| 8805ff97 | IC exe capabilities from `--list-rules --json` (K-P9-6 envelope), not `--help` |
| 88122c8a | R0-7-EOL: `.gitattributes` `scripts/specs/** text eol=lf` |
| 7f3bd88c | `atx.research-wave/v2` kinds (K-P9-9), `wave_kinds.py`, `wave_stage_kinds.py`, 10 kind fixtures |
| 0e6baa28 | D1 deviation: `inputs.theme_registry` passes `--theme-registry` (+ sha pin) to the u and w passes |
| fed4d4cb | A2 note: G-P5 allowlist row + frozen pair for the fields seal rule (cross-lane, T1's guard) |
| 11a0e6e4 | E1-N4: neutral exe notes (named by the exes_sha256 pin rule, not a call site) |
| fa7d5805 | WIP: sticky exes_sha256 fix (wave re-locks) + content digest drops nested time keys |

## Brief / carry items
Done (committed, tests pass):
- **M1a-RED.** Root cause: prereg item 4 applies the field rule "at the lock, to the registry of record". The lock is
  H-F = X-5, commit `a5df67ae`. The test read the live registry, which includes Y-S (`a5914373`, after the lock). That
  wave added so_wang_rev (ea_window_pre5), dato (noa_lag4) and ins_cluster (ins_cluster_buy). Fix:
  `REGISTRY_OF_RECORD_WAVES` = the `added_in` waves at `a5df67ae` (v4 v6 v61 v70 v71 v80 v81 v8x2 v8x3 v8x4 v8x7).
  `registry_of_record()` asserts those waves are still present and filters to them. The constant comes from the
  registry at the lock commit, not from the failing set. Checked: the filtered live registry equals the registry at
  `a5df67ae` (ids and dsl). test_research_mine.py: 42 passed.
- **E1-N3 (required).** `Cycle.template_chain()`, `parent_spec()`, `introducer(phase)` and `inherited(phase)` are in
  `scripts/cycle/phases.py`. `introducer` walks up while the phase output name and out_root are equal and there is no
  suffix. `reuse_pin` names its owner as "the spec's", "the parent's" or "the ancestor's (k templates up)". Test:
  `test_wave_driver.py::test_inherited_output_judged_by_the_ancestor_that_made_it`, 3 levels G -> P -> C. The
  `cycle_resume.py` docstring is updated.
- **Split (move-only).** The `scripts/research_cycle.py` facade re-exports every name, and `__main__` sets
  `sys.modules["research_cycle"]`. Monkeypatch targets follow the globals into their defining module: the tests patch
  `cycle.phases.window_id` and `cycle.spec.INPUT_KEYS`. Identity: `--help` and the usage errors are byte-identical. The
  hash-only plan of all 65 committed specs, plain and `--screen` (3,509 lines), is identical after the split, after the
  dedupe and after theme_registry. Tool: Resolver `known=` pins, so no output is opened.
- **research_common.py.** It holds RUN_SUFFIX, `fmt_argv`, `sha256_file`, `execute`, `run_dir_of`, `free_run_dir`,
  `dirty_or_error`, `read_ledger` and `ledger_view`. Users: `research_spec`, `run_bounded_research`, `wave_steps`,
  `wave_context`, `cycle_resume`, `wave_stage_preflight`, `wave_scoreboard`. The wave reads `Cycle.phase_bases()`;
  run-dir naming is defined once.
- **K-P9-9 v2 manifest.** `scripts/wave_kinds.py` has the kinds, the blocks, a JSON-Schema subset validator,
  `rules:` validated against the exes' list-rules JSON (IC `atx.composition-rules/v1`, NAV `atx.nav-rules/v1` with
  `x-flag`), and the role, horizon, leverage and walk-forward blocks (walk-forward is schema only).
  `scripts/wave_stage_kinds.py` covers preflight, the field and role builds in the register stage (receipts are
  numbered by index, so no new stage is added) and the cell doc and path. `wave_manifest.py` accepts v1 or v2.
- **Fixtures.** `scripts/specs/p9/kinds/*.json` (10 kinds) + `templates/` (5). 8 runnable; walk-forward and
  new-cost-model are schema only (until C3 / D3).
- **D1-PSCHEMA.** `wave_kinds.checked_params` validates rule `params` against `params_schema` from `--list-rules --json`.
- **D1 deviation.** `inputs.theme_registry` -> `--theme-registry PATH --theme-registry-sha256 PIN` on u and w (spec
  `INPUT_KEYS` tail). The D1 fitter theme-order refusal is no longer vacuous for a spec that sets the input. It stays
  vacuous for specs without it (the built-in 13-theme default).
- **R0-7-EOL.** Done; blobs are unchanged.
- **A2 note.** G-P5 row: `atx-engine/tools/prepare_research_fields.py` `require_research_seal` <->
  `atx-engine/include/atx/engine/research/fields/reuse.hpp`, retire by P10. test_no_python_mirror.py: 7 passed.
- **SQL-11.** No SQLite index flags were added.
- **`--candidates` in MARGINAL_SPEC_FLAGS.** Not needed. It is already carried by `marginal.candidates` /
  MARGINAL_BUILT (S1). Nothing changed.
- **Carried minors done:** N4 (11a0e6e4). Sticky exes_sha256 (fa7d5805): `wave_stage_util.lock_exes` is true under
  `driver.lock_exes` OR when the parent spec carries `exes_sha256`, so the wave re-locks every cell it writes.
  Documented in `wave_manifest.py`. Test: the added block in `test_lock_exes_pins_and_verify_compares`. A manual
  `research_cycle.py add-alpha` outside a wave still copies the parent's pins (fails closed; re-lock by hand). Nested
  time keys (fa7d5805): `stage_chain.TIME_KEYS` = started_utc, seconds, wall_seconds, peak_mib, dropped at any depth by
  `time_free()`. This is opt-in (`receipt_digest: "content"`); no committed manifest uses it.

Not started / partial:
- **Judge row order under a budget (E1 minor).** NOT STARTED. Planned design: add `Wave.side_by_side(calls)` in
  `wave_context.py`. It runs `[(fn, *args)]` in a ThreadPoolExecutor. A thread-local buffer per call makes
  `note_process` and `console` (used by `Wave.run` and `wave_stage_kinds.run_build`) append to that buffer instead of
  writing. After `wait(jobs)`, flush the buffers in call order, then return `[j.result() for j in jobs]`, which raises
  the first failure in call order, as before. Then replace the pool block in `wave_stage_record.judge` (:160-165) with
  `w.side_by_side([(score,), (bundle_once, w, parent_nav, nav), (read_once, w, "book", "book", navs)])`. Keep driver
  log lines live (they are progress, not records). Test: a judge under `host_budget_mib` whose fake finishes book before
  summ must give the same `processes` order and console numbers as one that finishes in submit order.
- **HostClaims lock failure -> runner-error (E1 minor), `scripts/run_bounded_research.py:167-183`.** NOT STARTED. Fix
  per task-E1-review.md: in `acquire_lock`, treat PermissionError like FileExistsError. Reclaim a lock that is
  unreadable and older than a few seconds (mtime). Bound the lock wait by the remaining admission time and raise
  AdmissionTimeout (retryable), not RuntimeError.
- **K-P9-10 `attempt` / `build_type` (E1 minor).** NOT STARTED. For k >= 2, pass `--attempt k` for the existing `-run<k>`
  numberings; k = 1 keeps every argv. Note that `build_type: null` means the default equity (Debug) build. This changes
  the argv of the second and later attempts only. Get the PM's OK first, because it touches the plan identity of resumed
  cells.
- **P9 PM7-35 retirement (`wave_rules.py:50-51`).** NOT DONE, deferred by design: retire it only after root has the
  identity against B1's C++ predicate. B1 note: `gate_counts` != the gate's count (runner-sign-0 strings).
- **Code-keyed reuse of imported modules (E1 minor `wave_stage_cell.py:73-78`).** Not in the E2 carry list; untouched.
- **Full suite re-run.** Last full runs: seed 0 after the split, 375 passed / 4 skipped (874 s); seed 1 after the
  dedupe, 375 passed / 4 skipped (784 s). Not re-run since 7f3bd88c.
- **task-E2-report.md.** NOT WRITTEN. Write it from this file plus the report template in the dispatch contract.

## Next steps (in order)
1. Optional: the judge-order and HostClaims minors above (each about 30-60 min with a test). Otherwise list them as not
   fixed in the report, with this file's reasons.
2. Run the full suite with explicit paths, twice: `PYTHONHASHSEED=0` and `=1`:
   `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests atx-engine/tools/test_stage_chain.py`.
   It takes about 15 min per seed. Afterwards, kill only your own leftover python children.
3. Re-run the hash-only plan identity, if the scratch tool is still there. Otherwise root does it (see below).
4. Write `task-E2-report.md`, `git add -f`, commit. Then send the PM the short final message.

## Tests (last result)
- test_research_mine.py 42 passed. test_wave_kinds.py (new) 42 passed. test_research_wave.py + test_wave_kinds.py 57
  passed. Wave suites after the v2 wiring: 86 passed. test_no_python_mirror.py 7 passed. test_stage_chain.py 9 passed.
  test_wave_driver.py: `-k lock_exes`, `-k receipt_digest` and the N3/N4 tests pass (the whole file was not re-run
  after fa7d5805).
- New or changed tests:
  - test_wave_driver: `test_inherited_output_judged_by_the_ancestor_that_made_it`, plus the sticky-pin block in
    `test_lock_exes_pins_and_verify_compares` and the N4 label assertion.
  - test_research_cycle: `test_theme_registry_input_reaches_the_ic_passes`, plus the list-rules capability fake.
  - test_stage_chain: nested time values in `test_content_digest_chains_without_time_keys`.
  - test_wave_kinds.py: 42 tests, all new.

## Files
- Owned (E1's inherited + brief):
  - `scripts/research_cycle.py`, `scripts/cycle/*`, `scripts/research_common.py`, `scripts/cycle_resume.py`
  - `scripts/research_spec.py`, `scripts/run_bounded_research.py`
  - `scripts/wave_*.py`, `scripts/wave_kinds.py`, `scripts/wave_stage_kinds.py`
  - `atx-engine/tools/stage_chain.py` + its test
  - `scripts/specs/p9/kinds/**`
  - tests: `test_research_cycle.py`, `test_research_spec.py`, `test_wave_driver.py`, `test_wave_kinds.py`,
    `test_research_mine.py` (carry)
- Cross-lane:
  - `scripts/tests/test_no_python_mirror.py` (T1): one ALLOWLIST row and one FROZEN_PAIRS pair, appended at the tails.
  - `.gitattributes`: one line + a comment.
  - `backtest_integrity.py` (B2) was NOT touched.

## Decisions
- Moved long lines are kept verbatim (move-only). New code wraps at 120.
- The `stage_chain` / `backtest_integrity` sha256 copies stay for layering (atx-engine/tools must not import scripts/).
- No record writer moved into `cycle/`. `gate.py` holds the verdict and ledger hooks (relevant to SQL3).
- The composition-rule fixture keeps `rule = <name>` only when the schema pins a const. Params are checked against the
  exe's schema, never a local copy.
- Sticky pins: the wave re-locks (one rule, fail-closed outside a wave). I did not choose stripping pins in
  derive/resolve, because stripping would weaken the N3 ancestor judging.

## Open questions for the PM
1. Is the K-P9-10 `--attempt k` argv change (k >= 2) wanted now? It moves the plan of resumed multi-attempt cells.
2. new-combination-rule fixture: it is refused until D2 corrects `params_schema` (D1's schema). Is that acceptable for
   the merge, or should the fixture wait for D2?
3. NAV rule fixtures (new-nav-rule, new-horizon) need C1's `atx.nav-rules/v1` exe (C1 is not in base `1239a5ff`). Does
   root run them after the C1 merge?

## Traps for the next agent
- The `rtk` shell hook rewrites grep/sed/git output: use the Grep tool or `rtk proxy <cmd>`. Python heredocs with
  backslashes break: use Write/Edit.
- Blind rule: do not open any 2020-2023 return / IC / Sharpe / turnover / NAV output. `.superpowers/sdd/platform-v8-*/
  waves/*/wave-result.json` are such outputs: keep greps out of them.
- Monkeypatching after the split: patch the defining module (`cycle.phases`, `cycle.spec`), not `research_cycle`.
- The tiny-world kind fixtures name the parent `../../../../../tiny.json`. The tiny-world `tiny.json` has no
  `summ.ledger`, so the wave preflight refuses on it without the wave harness: root must supply the harness.
- The plan-identity snapshot: compare only the 130 original keys. The p9 kind templates add 10 new keys.
- The full suite takes about 15 min per seed. Run it in the background with explicit paths only (PY-HYG).

## Identity checks root must run
- Run `research_cycle.py plan` and `research_wave.py plan` on every v8 spec and wave manifest, base vs head. They must
  be byte-identical.
- After the eol=lf checkout, re-check the spec pins (the blobs are unchanged; the working-tree bytes may change on
  Windows).
- Check that the real IC exe's `--list-rules --json` capabilities include list-rules, marginal, no-composition and
  theme-registry.
- Run the kinds fixtures on tiny-world under the wave harness (8 runnable).
