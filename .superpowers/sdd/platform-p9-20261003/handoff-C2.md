# Lane C2 hand-off: fix round 1 (owner stop, 2026-10-04)

**Superseded (RESUME 4): fix round 1 is complete; see `task-C2-report.md` "## Fix round 1". The state below is the
stop-time record only.**

The round-0 stop-time design record this file held is in git (`git show 08297183:.superpowers/sdd/platform-p9-20261003/handoff-C2.md`).

- Pool `C:/atx-wt/pool-15`, branch `feat/p9-c2-20261003`, base `ad406718`. Review `task-C2-review.md` (commit
  `4545cec4`, CHANGES_REQUIRED at `08297183`: R1, R2, R3 Required; S1-S7 Suggested); PM rulings C2-R2, C2-R3, C2-4b
  came with the fix-round dispatch.
- Head: the `wip(c2): fix round 1 at owner stop` commit (`c9024beb`) plus the commit adding this file. **The C++ part of
  the round is committed but NOT compiled and NOT run. The Python part (R1, R2, C2-4b) is written and saved but NOT
  applied to `scripts/wave_stage_cell.py`.** About 40 % of fix round 1.
- Processes: none running. Build trees: `build-equity/` and `build-equity-rel/` were reconfigured (configure only, no
  objects; `deps/equity-rel` fetched again); the round-0 receipts `build-equity/mega-p9-c2-{a..g}-*` are kept. The
  last memory check refused a build (commit 936 MiB < 2500), so no fix-round build was started.

## Per item

| item | state |
|---|---|
| R3 (C2-R3) `numpy_mean` = numpy at every n | **committed, not built.** `strategy_nav_calibrate.cpp`: `acc = 0.0; acc += pairwise_sum(block)` per 8192-value block (`reduce_buffer`), then `/ n`; header doc fixed. Gtest `CalibrateGross.NumpyMeanIsNumpysMeanAtEverySize` replaces `NumpyMeanIsNumpysPairwiseMean`: 20 bit patterns (series ramp and hash, n = 5 to 40000, across the 128 and 8192 boundaries) printed by `c2-numpy-mean-fixture.py` (this dir; numpy 1.26.4). The script also checks the model against numpy on 400 random series: 0 differ. The old unblocked mean fails ramp at 8193 and hash at 40000. Ledger candidate 2 in `task-C2-report.md` still states the old rule; amend it once the gtest is green |
| R1 ref inherits the calibrate flags | **written, not applied**: `c2-fix1/engine_block.py` `calibrated_doc` pins `ref.flags` = the cell's nav.flags and `ref.leverage` = its L unless the ref sets them (template: `change.set ref.flags / ref.leverage` only when the resolved spec has a ref). Test not written |
| R2 (C2-R2) plain cell of record / next_parent | **written, not applied** (`engine_block.py`). The run recipe `<cell>-cal.json` (= calibrated_doc; name `<name>-gm`; NAV `<output>-gm`) is locked, committed and run. After the checks, the cell of record `<cell>-gm.json` = `record_doc` = `WS.gm_doc(cell, L')` with nav.output = the engine NAV is written, locked and committed. It has the same name, so the same cycle dir (checked). match returns `cell_spec` = the run recipe, because verify's binding check and the judge's `research_cycle run` need the spec the NAV was made under. It also returns `record_spec`, `record_spec_digest` and `record_commit`. STILL TO DO: `scripts/wave_result.py` `build()` (E2's file, cross-lane, about 3 lines): `cell.spec` = `mt.get("record_spec") or mt["cell_spec"]`, `cell.spec_digest` = `mt.get("record_spec_digest") or <as now>`, and `cell.run_spec` / `cell.run_spec_digest` added only when `record_spec` is present (Python mode stays byte-identical). next_parent then follows cell.spec |
| C2-4b match_inputs pins | **written, not applied** (`c2-fix1/patch_cell.py`). The run stage (engine) records `spec_digest`, `w_combined` and `w_combined_sha256`. `w_combined` is `<ic.w_output>-<n>/train_combined.json` of the attempt `RC.Cycle.ic_attempt` picks. `match_inputs` pins all three in engine mode (STALE on change). The patch adds `import research_cycle as RC` |
| S1 100 columns | committed: the nav_replay.cpp line, both CMake comment tails, one Err line in calibrate.cpp |
| S2 reuse exclusions / X-5 keys | committed, not built: `CalibrateGross.ReuseAndItsExclusionsPublishThePlainRunAtTheFinalLeverage` in strategy_nav_replay_test.cpp. Cases: X-5 keys (price-risk-v1 + locate-in-aim + delta basis + liquidity cache) give reused true; adv-hold .5 gives false; hold band .5 + --emit-holdings gives false. Each must be byte-identical to the plain run at L', holdings included. Not covered: two-speed (needs saved sleeves in the fixture), the --max-bytes budget case (the loader's own charge is not exposed), warm start (the fixture's score_begin is 0) |
| S3 stats_ext wording | committed (now about the polynomials only; the folded exp/erfc/log/sqrt dependence is named) |
| S4 one leverage constant | committed: `aim_leverage_min/max` in `strategy_target_replay.hpp`, used by `validate_target_config` and by `nav_calibrate_leverage_min/max`. `risk_target::leverage_cap()` keeps its JSON integers 1 / 2 (a double would print `1.0` and move the `--list-rules` bytes) |
| S5 report wording | not done. Reword root step 3: compare leverage, gross, g_parent, g_calibration, leverage_calibration, corrected and the NAV bytes; spec, nav and the digests differ by design |
| S6 tape vs --max-bytes | not done: add a report note for root (read `construction_reused` in the X-5 gm pair) |
| S7 capability pin | committed: `NavBookRule.AimLeverageIsOptionalWithItsDefault` checks `calibrate-gross-pm6-6` |

## Exact next steps

1. Apply the Python patch: `"C:/Program Files/Python312/python.exe" .superpowers/sdd/platform-p9-20261003/c2-fix1/patch_cell.py`
   (exact-match replacements; each anchor is asserted; it reads `engine_block.py` from its own dir). Then check it parses:
   `python -c "import ast; ast.parse(open('scripts/wave_stage_cell.py', encoding='utf-8').read())"`.
2. Edit `scripts/wave_result.py` `build()` as in the R2 row.
3. `scripts/tests/wave_fixture.py`, in `FakeCycle.cycle`: for `--stop-after w`, write `<w base>-1/summary.json`
   (`{"status": "complete"}`) and `<w base>-1/train_combined.json` before `return self.ok(args)`. The w base is
   `c.out(spec["ic"]["w_output"])`.
4. `scripts/tests/test_research_wave.py`:
   - Update `test_engine_gross_match_is_the_python_match_in_one_nav`: cell.spec is now the plain `lib-w1b-gm.json` at L'
     without `--calibrate-*`; the run recipe is `cell.run_spec` = `lib-w1b-cal.json`, which carries the flags.
   - (a) R1 test: run `calibrated_doc` on a doc with a `ref` section (for example `scripts/specs/v8/lib-v80.json`).
     Assert `ref.flags` = the original nav.flags, and the ref nav research_cycle forms
     (`dict(nav, **{k: ref[k] for k in ("rule", "leverage", "flags") if k in ref})`) has no `--calibrate-*` and the
     old L.
   - (b) R2 test: run both modes with `dsr=0.05` (accepted). Beside next_parent, write a child template
     `{"schema": research_spec.TEMPLATE_SCHEMA, "name": "child", "parent": <next_parent file name>, "change": {"set":
     {"nav.output": "out/nav-child"}}}` and `RC.load_spec` it. Assert nav.leverage and nav.flags are equal between the
     modes and carry no `--calibrate-*`.
   - (c) C2-4b test: run an engine wave `--until match`, rewrite the w combined file, run again. Expect exit 3 and
     `STALE: run recorded w_combined_sha256` (pattern: test_wave_review.py:264).
5. pytest (explicit paths): `scripts/tests/test_research_wave.py`, then the 8 wave suites listed in the report.
6. Builds (check the gate first; root has priority):
   - `research-build.ps1 -Tag p9-c2-h -Targets "atx-impl-strategy-target-tests,atx-equity-strategy-targets"` (equity-dev),
   - then `-Tag p9-c2-i -Preset equity-rel -Targets atx-impl-strategy-target-tests`.

   Fix compile errors: the C++ edits were re-read but never compiled. Watch `std::min` with `usize` in calibrate.cpp,
   `aim_leverage_min` visibility in strategy_target_replay.cpp's namespace, and the new test's lambdas. Run
   `CalibrateGross.*:NavBookRule.*:BookNormalScore.*`, then the whole binary, in both builds. The S2 test may need
   fixture tuning: a skipped neutralization on 48 rows is fine, but the books must build (`g > 1e-3`).
7. Append "Fix round 1" to `task-C2-report.md`: per item what changed and its tests; the R2 design (run recipe vs cell of
   record) and the wave_result cross-lane edit; amend ledger candidate 2 to the blocked rule; the S5 wording; the S6
   note. Commit (`git add -f` for the sprint dir). Then delete the `build-equity*` object trees and `deps/equity-rel`
   again, keeping the receipts.

## Open questions

1. R2 design: the cell of record is the plain `-gm` recipe, but verify and judge must keep running the run recipe
   (`-cal`), because research_cycle binds the NAV to the spec digest and argv that made it
   (cycle_resume.check_binding). A `research_cycle run` of the plain `-gm` refuses that NAV, which is correct since that
   argv did not make it. Is this acceptable, or should the binding learn the calibrate flags (cycle_resume.py, not
   this lane's file)?
2. The engine NAV dir keeps the requested-L token in its name (`...-L1.1474-gm` even when L' is 1.1818). Renaming it
   would need L' before the run. Is this acceptable?
