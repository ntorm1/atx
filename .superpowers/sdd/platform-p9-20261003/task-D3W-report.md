# Task D3W report: walk-forward fit, v3 weights in the IC runner, the fitter post-step, ALCOMB-6/-7, D2-9, D2R-3

| | |
|---|---|
| Outcome | APPROVE at `1e6c4336` (review `f0726acb`: 0 Required, 10 Suggested; ruling D3W-S) |
| Pool / branch | `C:/atx-wt/pool-23`, `feat/p9-d3w-20261005` (created at `899b5811`; R8 resume at `23e650e6`) |
| Head | `1e6c4336` (code; C++ unchanged since `c7549d6a`); above it `01770e76` (DIRTY-1a handoff commit) and `f0726acb` (review, the APPROVE SHA root merges, MERGE-SHA); placeholders filled by D3W-PB (S-1) |
| Root merged | `1b64ce45` (merge `cec719a0`), then `fd084e78` (merge `c7549d6a`); root's later head `685e238b` is docs only and was not merged (root's code head stays `fd084e78`); no conflict |
| Rulings | K-P9-8, D3-1, D3-2, D3W-EVAL, ABS-1, D3W-PB, ALCOMB-4 .. -7, D2-1, D2-9, D2R-3, P12, PERM-1, ENV-1, HOST-2 |
| Builds | `p9-d3w-a` .. `-g` Debug (`equity-dev`), `p9-d3w-h` Release (`equity-rel`); receipts `build-equity*/mega-p9-d3w-*-receipt.json` (copied with the build logs to `d3w-logs/` by D3W-PB before the object trees were deleted) |
| Evidence | `d3w-logs/` beside this report (logs, fingerprints, `tools/`) |

## Commits (after `23e650e6`)

| SHA | What |
|---|---|
| `cec719a0` | merge root `1b64ce45` |
| `ccddcb82` | gtest reds fixed: walk-forward fixtures feasible on every row (a linear 6-member ic-shrink case and fixture case 1); `CompositionWeightRows` second row drops b1 |
| `a4a5baf4` | item 4: the fitter `--walk-forward` post-step (C++ rows into atx.dsl-composition-weights/v3) |
| `fec2c7b8` | item 5, ALCOMB-7: the C++ fit records the mom-volman sleeve; `--engine-fit --mom-volman` writes it |
| `cbdcb9ca` | the sleeve gtest uses theme-tsmom-v1's own window constants (lag 3, lookback 252) |
| `5514553e` | item 6, ALCOMB-6: the IC exe writes the sleeve series before / after scaling; `atx-research-eval --vol` |
| `3e35ff2e` | item 7, D2-9: S-b tests, S-d `check_factor_series` refusals, S-c strict int/float in `parent_check` |
| `2cdb4c51` | item 8, D2R-3: S1 / S2 assert the exact `unrecognized arguments: <token>` |
| `7fcb5ac8` | `WeightsV3` refusal loop reports every accepted case (teeth showed it stopped at the first) |
| `c7549d6a` | merge root `fd084e78` |
| `1e6c4336` | `RealExecutables`: `--walk-forward` rows and the mom-volman sleeve on the built exes |
| `01770e76` | DIRTY-1a: handoff as it stood at STOP 8 (committed by the reviewer) |
| `f0726acb` | first review of `1e6c4336`: APPROVE, 0 Required, 10 Suggested |

## Items: file:line, test, log

1. **Merge root.** `cec719a0` (`1b64ce45`), `c7549d6a` (`fd084e78`); root `685e238b` (docs only) not merged.
2. **C++ walk-forward fit (K-P9-8, D3-1).**
   - `atx-engine/include/atx/engine/research/composition/fit.hpp:200-226`: constants `walk_forward_expanding`, lag 2,
     step 21, min_decisions 21, `walk_forward_max_rows` 4096; `WalkForwardSpec`, `WalkForwardRow`, `fit_walk_forward`.
   - `atx-engine/src/research/composition/fit.cpp:379`: row j is the full composition fit on the TRAIN decisions
     d <= j - lag. Refusals: window, lag < 2, step 0, min < 2, theme-tsmom / theme-resid in the spec, no window ever
     fills, more than 4096 rows, a row whose fit refuses (names its decision and session). `:409` a row records no
     sleeve.
   - `atx-engine/src/research/composition/composition_cli.cpp:190` `walk_forward_json`; `:379` parses
     `fit --walk-forward expanding --lag N --step N [--min-decisions N]`; `:325` writes the block only with the flag.
   - Tests `atx-engine/tests/research/research_composition_fit_test.cpp`: `:583` `WalkForward.ExpandingMeanClosedForm`,
     `:622` `.NoRowUsesFutureDecisions`, `:676` `.RefusesBeforeAnyRow`, `:710` `.CliWritesTheRowsAndRefuses`.
   - Logs: `r8-gt-a-comp-*.log` (2 red: infeasible fixture rows), `r8-gt-b-comp-*.log` (14/14), teeth E1-E3
     `r8-teeth-cpp-engine.log`.
3. **IC runner reads v3 (per-date weights).**
   - `atx-impl/src/strategy_ic_admission.cpp:31` `weights_schema_v3`; a `walk_forward` block without v3 is refused.
   - `atx-impl/src/strategy_ic_rules.cpp:695` `composition_walk_forward` (shape and refusals; refused beside
     theme_redistribution, theme_residualise, theme-tsmom-v1); `:622` `walk_forward_stages` (row boundaries merged
     with the volman blocks).
   - `atx-impl/src/strategy_ic_composition.cpp:533` `IcComposition::set_weight_rows`: before the first row the book
     is flat; a candidate no row weights is skipped; `:347` `weight_at(d)` per date.
   - `atx-impl/src/strategy_ic_runner.cpp:145` manifest `composition_walk_forward`.
   - Tests `atx-impl/tests/strategy_ic_runner_test.cpp:4903` `WeightsV3.SingleRowEqualsV1Bytes`, `:4933`
     `.RowsSwitchAtTheirSessionAndTheBookIsFlatBefore`, `:4994` `.RefusalsPrecedeAnyPayloadOrOutput`;
     `atx-impl/tests/strategy_ic_composition_test.cpp:768`
     `CompositionWeightRows.RowInForcePerDateAndFlatBeforeTheFirst`.
   - Logs: `r8-gt-a-ic-*.log` (1 red: two rows whose rank composites had the same bits), `r8-gt-b-ic-*.log`,
     `r8-gt-e-ic-*.log`; teeth R1-R3, C1 `r8-teeth-cpp-ic.log`, `r8-teeth-cpp-ic-r1r3.log`.
4. **Fitter post-step (D3-2 names).**
   - `atx-impl/tools/composition_engine_fit.py`: constants `:99-114` (`WALK_FORWARD_RULE`, `POST_STAGE`,
     `POST_ARMED`, `WALK_FORWARD_FIT`, `VOLMAN_FIT`, `WEIGHTS_SCHEMA_V3`, `VOLMAN_SOURCE`, `POST_ONLY`,
     `WALK_FORWARD_REFUSED`); flags in `add_argument` `:171-183`; `walk_forward` `:185`; `check_walk_forward` `:202`
     (before any compute); `check_args` `:231` refuses a post-step flag outside `run_post_step` (`POST_ONLY`);
     `check_fit` `:520` adds the walk-forward argv to the one C++ call and keeps the rows; `walk_forward_rows` `:666`
     (shape check); `weights_v3` `:692`; `PostStepHooks` `:749`; `run_post_step` `:761`.
   - `atx-impl/tools/fit_composition_weights.py:2817`: `main()` routes through `run_post_step` when
     `composition_engine_fit.post_step(args)`; else `fit()` exactly as before.
   - Tests: new `atx-impl/tools/test_fit_walk_forward.py` (`WalkForwardEndToEnd :153`, `MainRoutes :235`,
     `Refusals :291`, `MomVolmanEngineFit :441`, `WeightsV3 :545`, `RealExecutables :589`; the last runs the built
     exes when `ATX_RESEARCH_ADMISSION_EXE` and `ATX_RESEARCH_COMPOSITION_EXE` are set, else skips).
   - Fingerprint: the fitter's `OUTPUT_PRODUCERS` stays
     `d28a30895ad1393e512d3a20384af025063675e9a2617611ee9cae41e17304a2` (`fingerprints-before.json`,
     `fingerprints-final.json`).
   - Logs: `r8-item4-pytest-fitter.log`, `r8-item4-flag-absent-identity.log`, teeth M-P1 .. M-P6
     `r8-teeth-python.log`.
5. **ALCOMB-7 (the C++ sleeve).**
   - `fit.hpp:135` `FitSpec::mom_volman`; `:165` `CompositionFit::volman_sleeve`; `:174` `mom_volman_rule`,
     `mom_volman_theme`, min 128 / max 65536 decisions.
   - `fit.cpp:142` `theme_sleeve` (one helper shared with `theme_tsmom_schedule`, same operation order); `:160`
     `mom_volman_sleeve` (refuses "has no weighted member" and the decision bounds); `:274` refused on an aim
     parent or beside theme-tsmom-v1 / theme-resid-v1; `:371` the block.
   - `composition_cli.cpp:286` reads spec `mom_volman`; `:183` writes `mom_volman {theme, sleeve}` only when asked.
   - Fitter: `fit_spec :382` adds `"mom_volman": true` only when on; `volman_sleeve :406` (Python side);
     `expected_fit :421` holds the C++ sleeve to it under P12 in check 3; `engine_sleeve :732` writes the C++ sleeve
     (`theme_schedule.sleeve`, `provenance.mom_volman.sleeve_source = VOLMAN_SOURCE`).
     `composition_mom_volman.py`: the engine-fit refusal and `ENGINE_FIT_REFUSED` removed (its fingerprint is
     unchanged).
   - Tests: `research_composition_fit_test.cpp:839` `MomVolman.SleeveIsTsmomStepOneForPriceMomentum`, `:897`
     `.RefusalsAndWalkForwardRows`, `:951` `.CliWritesTheSleeveBlockOnlyWhenAsked`; pytest `MomVolmanEngineFit`;
     `test_composition_mom_volman.py` (engine-fit outside main() now expects `POST_ONLY`).
   - Logs: `r8-gt-c-comp-whole.log` (1 red: wrong window constants), `r8-gt-d-comp-whole.log` (17/17),
     `r8-item7-flag-absent-identity.log`; teeth E4, E5, M-P6.
6. **ALCOMB-6 (sleeve series; the eval half per D3W-EVAL).**
   - `atx-impl/src/strategy_ic_detail.hpp:183` `volman_sessions`, `volman_sleeve`;
     `atx-impl/src/strategy_ic_mom_volman.cpp:147-150` (cross-lane) keeps them;
     `atx-impl/src/strategy_ic_rules.cpp:788` `write_mom_volman_sleeves`: per scored role
     `<role>_mom_volman_sleeve_raw.csv` and `<role>_mom_volman_sleeve_scaled.csv` (header `session_ns,net_return`,
     17 significant digits, rows from the first block's session within the role, scaled = raw x scale_b; nothing
     written when there is no row); `atx-impl/src/strategy_ic_runner.cpp:707` calls it after `add_mom_volman_trace`.
     `summary.json` unchanged.
   - Eval verb (cross-lane append): `eval_report.hpp:108` `EvalRequest::vol`; `eval_cli.cpp:118` `--vol`;
     `eval_report.cpp:613` `vol_json` (sessions, mean_daily, sd_daily ddof 1, vol_annual = sd x sqrt(252),
     sharpe_annual = house_sharpe); `:641` `config.vol` only with the flag; `:659` `net_vol` per cell.
   - Tests: `atx-impl/tests/strategy_ic_mom_volman_test.cpp:347` `MomVolman.SleeveFilesBeforeAndAfterScaling`;
     `atx-engine/tests/research/research_eval_tie_test.cpp:374` `EvalVerb.VolFlagAddsNetVolOnly`.
   - Logs: `r8-gt-e-ic-*.log`, `r8-gt-e-eval-whole.log` (25/25); Release `p9-d3w-h`: `r8-rel-atx-engine-research-eval-tests.log` (25/25) and the synthetic `--vol` run through the Release exe `r8-rel-eval-vol-synthetic.log` (exit 0).
7. **D2-9.** `composition_engine_fit.py:252` `check_factor_series` reads through `_read` / `_json` and requires
   candidate rows to be objects (S-d); `:469` `compare_values(..., strict=False)`, `:479` strict int/float (S-c);
   `composition_resid.py:253` `parent_check` uses `strict=True`. Tests `test_composition_engine_fit.py:604`
   `test_strict_types_refuse_an_integer_against_an_equal_float`, `:617` `test_parent_check_engine_mode_is_strict`,
   `:642` the S-b branches of `test_compare_admission_csv`, `:658` `test_run_exe_refusals`, `:675`
   `test_factor_series_reads_are_refusals`. Teeth M-P7, M-P8.
8. **D2R-3 S1 / S2.** `atx-impl/tools/test_fit_composition_weights.py:753` (S1) and, cross-lane,
   `scripts/tests/test_research_spec.py:941` (S2) assert `unrecognized arguments: <token>` exactly.

## Tests (final head)

Head `1e6c4336`; its C++ equals `c7549d6a` (the Release build's source), so the C++ counts below hold for it.

| run | result | log |
|---|---|---|
| EXE-CUR, Release `p9-d3w-h` (source `c7549d6a`, 7 exes) | all match | `r8-rel-exe-cur.log`, `receipt-p9-d3w-h.json` |
| `atx-engine-research-composition-tests` (Release, whole) | 17 / 17, exit 0 | `r8-rel-atx-engine-research-composition-tests.log` |
| `atx-engine-research-eval-tests` (Release, whole) | 25 / 25, exit 0 | `r8-rel-atx-engine-research-eval-tests.log` |
| `atx-impl-strategy-ic-tests` (Release, whole) | 231 / 231, exit 0 | `r8-rel-atx-impl-strategy-ic-tests.log` |
| fitter / composition pytest files (exes `p9-d3w-h`) | 305 passed, 87 subtests, exit 0 | `r8-final-pytest-fitter.log` |
| `test_fit_walk_forward.py` (exes `p9-d3w-h`) | 23 passed, 41 subtests, exit 0 | `r8-rel-pytest-walk-forward-file.log` |
| `-k RealExecutables` | 2 passed, exit 0 | `r8-rel-pytest-real-exes.log` |
| fitter flag-absent identity vs root `fd084e78` | 17 files identical; 4 differ only at the two `producer_fingerprint` fields | `r8-final-flag-absent-identity.log` |
| `scripts/tests` seed 0 (`c7549d6a` tree; `1e6c4336` touches no `scripts/` file) | 519 passed, 5 skipped, 0 failed, exit 0 | `r8-scripts-tests-seed0.log` |
| `scripts/tests` seed 1 | VOID under ENV-1: 177 failed / 342 passed / 5 skipped; every failure a child exiting 3221225794 (0xC0000142) or a chain downstream of one; deferred to the gate (TEST-1) | `r8-scripts-tests-seed1.log` |
| `atx-engine/tools/test_stage_chain.py` | 10 passed, exit 0 | `r8-stage-chain.log` |
| eval `--vol`, synthetic CSVs, Release exe | exit 0 | `r8-rel-eval-vol-synthetic.log` |

## Teeth (working-tree mutations, each reverted; `d3w-logs/tools/teeth_*.py`)

- C++ (Debug `p9-d3w-f` / `-g`, receipts `receipt-p9-d3w-f-MUTATED.json`, `-g-MUTATED.json`): E1 window, E2
  tsmom / resid in a row spec, E3 the 4096-row bound, E4 the volman parent rule, E5 the volman decision bounds,
  R1 the grouping refusal, R2 strictly increasing sessions, R3 an empty row, C1 rows under redistribute. Each turns a
  named test red (`r8-teeth-cpp-engine.log`, `r8-teeth-cpp-ic.log`, `r8-teeth-cpp-ic-r1r3.log`). The Debug binaries
  were left mutated; every final gtest count above is from the Release build `p9-d3w-h`.
- Python M-P1 .. M-P8 (`r8-teeth-python.log`): every mutation red, each file restored byte for byte.

## Flag-absent identity: procedure for root

1. C++: run the whole `atx-engine-research-composition-tests`, `atx-impl-strategy-ic-tests` and
   `atx-engine-research-eval-tests`. Every pre-existing case passes unchanged: the runner's v1 / v2 byte tests and the
   mom-volman, theme-tsmom and theme-cov-hedge cases need no edit (ALCOMB-4 / -5). `WeightsV3.SingleRowEqualsV1Bytes`
   proves one v3 row equals the v1 bytes; `EvalVerb.VolFlagAddsNetVolOnly` proves the eval JSON without `--vol` is
   unchanged.
2. Python: `git archive <root> atx-impl/tools atx-engine/tools atx-impl/strategies | tar -x -C BASE`; run
   `d3w-logs/tools/ident.py BASE/atx-impl/tools WORK OUT_BASE`, then `ident.py <merged>/atx-impl/tools WORK OUT_HEAD`
   with the SAME `WORK` (the runner summary records paths); then `cmp.py OUT_BASE OUT_HEAD`. Expected
   (`r8-final-flag-absent-identity.log`, root `fd084e78`): 17 files byte-identical. The only differences are the
   code-identity fields `provenance.engine_fit.producer_fingerprint` (engine-fit runs: `verify`'s closure holds
   `check_fit` and `compare_values`) and `provenance.resid.producer_fingerprint` (`--theme-resid` runs: D2-9 S-c in
   `parent_check`). No expected hash was edited.
3. Fingerprints (`d3w-logs/tools/d3w-fingerprints.py`, cwd `atx-impl/tools`): versus root only
   `composition_engine_fit` (`79966210...` to `a6dd0da5...`) and `composition_resid` (`d7c911ac...` to `9c48f390...`)
   move; the fitter's `OUTPUT_PRODUCERS` `d28a3089...` and every other module are unchanged
   (`fingerprints-root-fd084e78.json`, `fingerprints-final.json`).

## New literals, flags, keys

- Literals: `walk-forward-v1`, `expanding`, `mom-volman-v1` in C++ (`fit.hpp:174`), `.engine-stage`. No new
  `atx.<name>/v<n>` (`atx.dsl-composition-weights/v3` was already in `classes.json`).
- Flags: `atx-research-composition fit --walk-forward expanding --lag N --step N [--min-decisions N]`; fitter
  `--walk-forward expanding --walk-forward-lag N --walk-forward-step N [--walk-forward-min-decisions N]`;
  `atx-research-eval --vol`.
- Keys: fit spec `mom_volman` (only when on); `composition_fit.json` `walk_forward {...}` and
  `mom_volman {theme, sleeve}`; weights v3 top-level `walk_forward {rule, window, lag, step, min_decisions,
  rows:[{from_session, weights:{every library id}}]}` and `provenance.walk_forward {rule, composition_fit_sha256,
  producer, rows:[{decision_index, from_session, fit_decisions, last_fit_index, last_fit_session}]}`;
  `provenance.mom_volman.sleeve_source`; fitter summary `walk_forward {rule, rows, first_from_session,
  last_from_session}`, `output`, `files`, `weights_sha256`; IC `weights_summary.walk_forward`,
  `method_recipe.composition_walk_forward`, manifest `composition_walk_forward`; IC output files
  `<role>_mom_volman_sleeve_raw.csv` / `_scaled.csv`; eval `config.vol` and per-cell `net_vol`.

## Cross-lane edits (listed)

- `atx-impl/src/strategy_ic_mom_volman.cpp:147-150`: `pinned.volman_theme = momentum;`, the sleeve and its sessions
  kept.
- `atx-impl/tests/strategy_ic_mom_volman_test.cpp:347`: one appended test.
- `scripts/tests/test_research_spec.py:941` (D2R-3 S2).
- The eval verb (D3W-EVAL): `atx-engine/include/atx/engine/research/eval/eval_report.hpp`, `eval_cli.hpp`,
  `atx-engine/src/research/eval/eval_report.cpp`, `eval_cli.cpp`, `atx-engine/tests/research/research_eval_tie_test.cpp`.

## For PRE (the P9-W block)

- **Fit mode:** `--engine-fit` with `--walk-forward expanding --walk-forward-lag 2 --walk-forward-step 21`
  (min_decisions defaults to the registered 21). Refused with `--theme-tsmom`, `--theme-resid` and `--era`, and
  without `--engine-fit` (`WALK_FORWARD_REFUSED`).
- **The per-date weights producer:** the C++ `atx-research-composition fit --factors D --spec S --output O
  --walk-forward expanding --lag 2 --step 21 --min-decisions 21` (one call: the same call check 3 verifies), then
  the fitter's `run_post_step` writes atx.dsl-composition-weights/v3 (the full-TRAIN weights, signs and blocks
  unchanged). The IC runner holds the book flat before the first row; P9-W's dSR includes that stretch (D3-1).
- **`composition_fit.json` `walk_forward`:** `{rule: "walk-forward-v1", window: "expanding", lag, step,
  min_decisions, rows: [{decision_index, from_session, fit_decisions, last_fit_index, last_fit_session, weights:
  [spec member order]}]}`.
- **P9-M's sleeve line reads:** `atx-research-eval --vol --daily raw=<role>_mom_volman_sleeve_raw.csv --daily
  scaled=<role>_mom_volman_sleeve_scaled.csv` (each cell's `net_vol.vol_annual` and `net_vol.sharpe_annual`).
  PRE-38 stands (the realised vol ratio from the NAV exe's `ann_vol`).
- **ALCOMB-6:** the sleeve files exist only for a mom-volman-v1 run with at least one recorded block row in the role.
- **Stale registration text:** `scripts/specs/p9/rules/mom-volman-v1.json:50` still says `--engine-fit` is refused
  with `--mom-volman`; after ALCOMB-7 it is accepted and writes the C++ sleeve. Not edited (registered file).

## For ABS

`atx-impl/src/strategy_ic_composition.cpp:342` (`IcComposition::add`): the per-member skip
`if (sign == 0 || (dated ? p.row_weighted[index] == 0 : p.weights[index] == 0))` is where a whole-member absent skip
goes. `:347` `weight_at(d)` is the per-date hook (a weight of 0 there skips that member on that date);
`add_standardised` (`:411`) has its own `weight_at` (`:419`), so a per-date absent skip goes in both.

## For part B (what D3W-PB starts from)

- `fit.cpp:264-269` dispatches the four rules; `ew-theme-aim-v2` is refused there today ("composition must be ...").
  `FitSpec` members carry `gain` (`fit.hpp:125`); the aim v1 path multiplies scores by gains (`fit.cpp:316`).
- `fit.cpp:142` `theme_sleeve` is the shared sleeve helper; the walk-forward row spec clears `mom_volman` (`:409`).
- Fitter side: `fit_spec :382`, `expected_fit :421`, `kernels`, and `check_fit :520`'s `blocks` list are where a new
  block is declared, compared under P12 and written.
- `composition_resid.parent_check` now compares strictly (D2-9 S-c). Resid diagnostics and the D2-3 deletion are
  untouched.

## Deviations

- ALCOMB-7: the C++ sleeve is written by the post-step (`engine_sleeve`) after check 3 holds it to the Python sleeve
  under P12, so `fit` / `fit_prior` (OUTPUT_PRODUCERS) stay byte-identical (D3 design, D2-1).
- The `composition_engine_fit` and `composition_resid` module fingerprints move (code identity; identity step 3).

## Ledger candidates

- `rtk diff` printed "Files are identical" for two differing JSON files; use `rtk proxy diff`.
- The fitter's flag-absent identity must run base and head in the same WORK directory: the runner summary records
  paths, so `admission_sha256` / `runner_summary_sha256` differ otherwise.
- A walk-forward ic-shrink fixture needs a feasible member cap on every expanding window, not only on the full one.

## Permission refusals

None.

## Open questions

- `scripts/specs/p9/rules/mom-volman-v1.json:50` is stale after ALCOMB-7: amend through a registration note, or leave?
