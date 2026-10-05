# Handoff D3 (owner stop, part A in progress)

| | |
|---|---|
| Pool / branch | `C:/atx-wt/pool-20`, `feat/p9-d2-20261003` |
| Head | `0389e868` `WIP(d3): ...` (this file is committed after it) |
| Root merged | `5c57c1ef` (merge `f2ff4954`, plain), then `9c7aa654` (merge `899b5811`, plain; brings C3 part A `0dbfa459`, so `x-flag-aliases` is in the tree: `atx-impl/src/book/nav_rules.cpp:74`, `:115-119`) |
| Rulings read | PRE-35 (lane split: part A = items 1-5; part B = item 6), W3-7, W3-8, C3-3, E2-17, E2-8, ALCOMB-4..7, D2-9, D2R-3, P19, P14, KINDS-TW, PRE-14 |
| Build | none. `build-equity` was configured (`atx-build.ps1 configure -Preset equity-dev`, exit 0, ~110 s, log `d3-configure.log` in this dir). No compile has run. The configured tree is KEPT (a resume needs it; no object files yet, small). |
| Tests run | none (owner stop came before the first run). The two edited Python files parse (`ast.parse`); nothing else is shown. |
| Part B (item 6) | untouched. Nothing parked. |

## Part A, per item

| Item | State | What exists / what is left |
|---|---|---|
| 1. Walk-forward v3 (C++ fit `--walk-forward`, runner reads v3, `WeightsV3.SingleRowEqualsV1Bytes`, ALCOMB-4/-5 identity) | **not started in code** (design done, below) | No C++ edited. |
| 2. `walk-forward` kind out of SCHEMA_ONLY | **partly** (WIP `0389e868`, untested) | `scripts/wave_kinds.py`: `SCHEMA_ONLY = ()`; `WALK_FORWARD_FLAGS`, `KIND_FLAGS` (owned by the kind via `table_flags`), `walk_forward_problems` fixed (the old check refused a `window` string), `walk_forward_flags`, `walk_forward_token`, `walk_forward_set` (renames fit.output, ic.w_output, nav.output `-wf<lag>s<step>[m<min>]`), `walk_forward_parent_problems` (parent fit must carry `--engine-fit`; refuses `--theme-tsmom`, `--theme-resid`, `--era`), `walk_forward_template`; `problems()` keeps "has no rule_cell" for walk-forward. `scripts/wave_stage_kinds.py`: `preflight_kind` walk-forward branch (flags derived, parent checked), `kind_constants`, `base_rule_cell` (template next to the parent spec), `preflight_kind_plan` line. Fixture `scripts/specs/p9/kinds/walk-forward.json`: description only. **Left:** `scripts/tests/test_wave_kinds.py` (remove `walk-forward.json` from `SCHEMA_ONLY_FIXTURES` at `:62`; `:199-200` "8 runnable + 2 schema only"; `:343-344`; replace `test_walk_forward_is_schema_only` `:1021` with a run-to-spec test whose world parent carries `--engine-fit` in fit.flags, plus refusals: parent without `--engine-fit`, parent with `--theme-tsmom`, a rule wave setting `--walk-forward` raw). The fitter flags themselves (`--walk-forward` etc.) do not exist yet (item 1). Expect reds in `test_wave_kinds.py` at HEAD until this is done. Root's C3 `new-cost-model` flip in the same `SCHEMA_ONLY_FIXTURES` tuple: merge conflict expected there. |
| 3a. C3-3 alias guard | **partly** (WIP, untested) | `wave_kinds.flag_aliases`, `alias_problems` (fail closed on a malformed or shared alias), aliases added to `table_flags` (owned); `raw_rule_flags(..., exempt)`. `wave_stage_kinds.rule_flag_guard`: `alias_problems`, `alias_exempt` (an alias whose row the wave names in `rules` may change), exempt removed from the resolved-cell comparison, `alias_beside_rule` (named row's x-flag plus its alias on the cell's nav.flags is refused). **Left:** tests against a NAV table carrying `x-flag-aliases` (add a `cost-sqrt-v2` row with `"x-flag": "--cost-law cost-sqrt-v2"`, `"x-flag-aliases": ["--cost-v2"]` to the test file's `NAV_TABLE` or a variant): raw `--cost-v2` in constants.flags / change.flags refused; `--cost` abbreviation refused; inherited `--cost-v2` in the parent passes; template removing it refused unless `rules` names `cost-sqrt-v2`; named row + alias left on list refused; malformed alias field refused. Teeth by reverting each check. |
| 3b. E2-17 `inputs.*` guard | **partly** (WIP, untested) | `wave_stage_kinds.inputs_routes`, `inputs_changed`, `inputs_refusal` (called from `rule_flag_guard` after `leverage_refusal`); `wave_kinds.KIND_INPUTS = {"field": (), "role": ("role",), "horizon": ()}`. Refuses every route (constants.set, change.set / unset, change.inputs, whole `inputs`) and any resolved difference, naming key and route. **Left:** tests (rule wave: `rule_cell.constants.set inputs.reference_combined` (the E2 O2 probe), template `change.inputs.x`, `change.unset inputs.x`, whole `inputs` set; role wave: `change.inputs.role` allowed, another key refused); check the committed role / horizon fixtures still pass. |
| 3c. Live-table check as a test | **not started** | Port `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/m2h-logs/e2-live-table-check.py` into `scripts/tests/test_wave_kinds.py` as a test skipped unless `ATX_EQUITY_BIN` names a bin with both exes; the live NAV table now has C3's trade-rate / cost-law rows (assert `alias_problems == []`, `flag_aliases == {"--cost-v2": "cost-sqrt-v2"}`). |
| 4. ALCOMB-7 / ALCOMB-6 | **not started** | Design below. |
| 5. D2-9 (S-1 test, S-2 refusal in `check_factor_series`, strict int/float in engine `parent_check`), D2R-3 (S1, S2) | **not started** | Files: `atx-impl/tools/composition_engine_fit.py:145-176`, `composition_resid.parent_check` / `composition_engine_fit.compare_values:337-368` (int vs equal float passes today at `:344-355`), `atx-impl/tools/test_fit_composition_weights.py:750`, `scripts/tests/test_research_spec.py:938` (cross-lane, listed in D2R-3). |

## Design decided (for the next agent; nothing of it is coded)

**Fingerprint constraint (important).** The fitter's `producer_fingerprint` is the AST closure of `fit` / `fit_prior`
(`fit_composition_weights.py:378`, `:434-441`; `atx-engine/tools/code_fingerprint.py`). Any edit inside `fit`,
`fit_prior` (including the nested `compose`) or a module-level name they reach moves `inputs.producer_fingerprint` /
`provenance.producer_fingerprint` in EVERY fit output: an identity miss for root. Editing a composition module's
`PRODUCERS` function (`composition_mom_volman.schedule`, `..._theme_erc.attach`, `composition_resid.apply`, ...) moves
that module's fingerprint in its outputs (ALCOMB-4/-5 forbid it for mom-volman / tsmom / hedge). `main`, `parse_args`
and everything in `composition_engine_fit.py` are outside the fitter's closure (an import name only), and
`composition_engine_fit`'s own fingerprint is written only under `--engine-fit`.

**Item 1, C++ engine** (`atx-engine/include/atx/engine/research/composition/fit.hpp`, `src/.../fit.cpp`; no CMake edit
needed): `WalkForwardSpec{window "expanding", lag 2, step 21, min_decisions 21}`; `fit_walk_forward(view, ids, spec,
wf)`: rows at j = first, first + step, ... < D, first = smallest j whose window (TRAIN decisions d with d + lag <= j)
holds min_decisions; each row = `fit_composition` on that window mask; refuse lag < 2, step 0, min_decisions < 2,
another window, `theme_tsmom` / `theme_resid` in the spec, > 4096 rows, any row's fit refusing (name j and session).
`composition_cli.cpp` `fit` gains `--walk-forward expanding --lag N --step N [--min-decisions N]` (lag and step required
with it; refused without it); `composition_fit.json` gains `walk_forward {rule "walk-forward-v1", window, lag, step,
min_decisions, rows [{decision_index, from_session, fit_decisions, last_fit_index, last_fit_session, weights (member
order)}]}`; flag absent: bytes unchanged. Gtests in `atx-engine/tests/research/research_composition_fit_test.cpp`:
`WalkForward.ExpandingMeanClosedForm` (ic-shrink row ICs = the expanding mean), `WalkForward.NoRowUsesFutureDecisions`
(perturb factors at d > j - lag: row j unchanged), CLI refusals.

**Item 1, runner** (`strategy_ic_admission.cpp:648-701`, `strategy_ic_detail.hpp:159-184`, `strategy_ic_rules.cpp:
606-628`, `strategy_ic_composition.{hpp,cpp}`): schema v3 accepted (`weights_schema_v3`); v3 requires a `walk_forward`
block `{rule, window, lag, step, min_decisions, rows: [{from_session, weights: {id: w}}]}` (1..4096 rows, from_session
strictly increasing, every library id, finite >= 0, a row may weight only candidates the top-level weights weight);
v1 / v2 refuse the block; theme_standardise etc. allowed under v2 or v3. Top-level `weights` = the full-TRAIN fit
(verify functions, signs, theme indices unchanged). `IcCompositionStages` gains `weight_rows [{begin, weights}]`;
`IcComposition` uses the row weight per date in `add` / `add_standardised` (skip a date whose row weight is 0: no
signed-zero bit change; dates before the first row add nothing, so the book is flat until the first fit). Under
standardise, `composition_stages` builds the masses in force as a schedule stage: a block at 0 with zero masses when the
first row starts later, then per row the theme sums of its weights (summed as `create` sums `std_mass`); with
mom-volman, merge boundaries and multiply price_momentum's row mass by the block's scale (needs one appended line in
`strategy_ic_mom_volman.cpp`: `pinned.volman_theme = momentum;`, a cross-lane edit); refuse v3 with theme-tsmom,
theme-resid and the redistribution (v6) grouping. Hedge and two-speed read the schedule masses unchanged. Recipe /
combined manifest / summary keys only for v3 (`composition_walk_forward`). `strategy_marginal_ic.cpp:267` keeps refusing
v3 (marginal reads the parent's pool, never the cell's weights). Gtest `WeightsV3.SingleRowEqualsV1Bytes` in
`atx-impl/tests/strategy_ic_runner_test.cpp` (fixture + `exact_outputs()`; a v3 file with one row at the first session
equal to the v1 / v2 weights gives byte-identical outputs; recipe differs only in `composition_weights_sha256`), plus
refusal tests before any payload (pattern `:837-890`).

**Item 1, fitter** (no edit inside `fit` / `fit_prior`): flags `--walk-forward expanding --walk-forward-lag N
--walk-forward-step N [--walk-forward-min-decisions N]` added in `composition_engine_fit.add_argument`; `check_args`
requires `--engine-fit` with them and refuses tsmom / resid / era. Under the flag, `verify` runs the C++ fit with the
walk-forward options (one exe call; the comparison ignores the extra block) and stores the C++ rows on `args` (the
Namespace main passes in). `main()` (outside the closure) calls a `composition_engine_fit` runner: fit into a staging
dir `<output>.wf-stage` (refuse a stale one), then build the v3 document (published doc + schema v3 + `walk_forward`
block with library-keyed row weights + `provenance.walk_forward`), publish atomically to `--output` (pending dir +
rename), remove the stage, fix the summary (output, files, weights_sha256, `walk_forward`). Flag absent: `fit()` as
today, every byte unchanged.

**Item 4.** ALCOMB-7: `FitSpec.mom_volman`, `CompositionFit` sleeve (the price_momentum sleeve: `theme_tsmom_schedule`'s
step-1 formula on the final weights, 0 outside TRAIN), JSON `mom_volman {theme, sleeve}`; `composition_engine_fit`
compares it under P12 (Python side recomputed with `composition_theme_tsmom.sleeves` from the state) and drops the
refusal in `composition_mom_volman.check_args` -- careful: `check_args` is not in that module's `PRODUCERS`
(`schedule`, `attach`), so editing it moves no fingerprint, but `ENGINE_FIT_REFUSED` text is a module constant: check
it is not reached by `schedule` / `attach`. Writing the C++ sleeve (ruling D2-1) goes through the same main()
post-step as walk-forward. ALCOMB-6: no eval change needed: `atx-research-eval --daily LABEL=CSV` takes
`session_ns,net_return`; the IC exe writes `<role>_mom_volman_sleeve_raw.csv` / `_scaled.csv` beside
`composition_schedule_trace` (`strategy_ic_mom_volman.cpp:164-187`), and the template line names those two files.

## Next step for a fresh agent

1. Read this file, PRE-35, the carry. Merge root's head again (`git merge feat/platform-p9-20261003`).
2. Finish 3a / 3b / 2 tests in `scripts/tests/test_wave_kinds.py`, run it and the whole `scripts/tests` (seeds 0, 1) and
   `atx-engine/tools/test_stage_chain.py` separately; commit (the WIP commit stays; a follow-up commit, no rewrite).
3. Item 1 C++ (engine, then runner), one build `p9-d3-a` (equity-dev is configured), gtests; then the fitter post-step
   and its pytest.
4. Items 4, 5; live-table test; report `task-D3-report.md`.

## Open questions

- min_decisions default (21 chosen in the design, from the windows set): the PM / PRE may register another value.
- Before the first walk-forward row the book is flat (no fit exists): the P9-W dSR includes that flat month; PRE may
  want it stated.
