# Handoff: lane AL-COMB (owner stop, 2026-10-03)

- Pool: `C:/atx-wt/pool-19`. Branch: `feat/p9-alcomb-20261003`. Base: `1239a5ff`.
- WIP commit holding all the code: `7c5c9a32942ec92b393a6d8d04abb38817a6b71c` (`wip(al-comb): ...`). This handoff file
  is committed on top of it. The tree was clean after the WIP commit.
- State: about 75% done. The code and tests are written. Python is green. **The C++ has never been compiled**
  (lanes do not build). Still missing: the spec files, the lane report and the themed commits.
- Processes: my only background job (the fitter pytest suites) had exited 0. I left no child process running.

## Brief / carry items

| item | state |
|---|---|
| (1) `theme-cov-hedge-v1`: kernel `atx-engine/include/atx/engine/combine/char_cov_hedge.hpp` + runner rule `strategy_ic_theme_hedge.{hpp,cpp}` as one row of D1's table | DONE (unbuilt) |
| (2) `mom-volman-v1`: price_momentum mass x target/sigma (126), running-mean target, as a schedule rule beside theme-tsmom | DONE (unbuilt) |
| Fitter flags `--theme-hedge theme-cov-hedge-v1`, `--mom-volman mom-volman-v1` (plugin modules) | DONE, pytest green |
| Gtests named by the brief: `CharCovHedge.ClosedForm2Names`, `.FutureReturnDoesNotChangePast`, `ThemeHedgeRule.FlagAbsentIdentity`, `MomVolman.StepVarianceClosedForm` (plus extras) | WRITTEN, never run |
| (3) Zero-trial diagnostic spec for lit section 8 Q5 | NOT STARTED |
| (4) Templates `scripts/specs/p9/templates/{theme-cov-hedge,mom-volman}.json` with `rules:` blocks; rule registration files with K-P9-11 keys | NOT STARTED (open question 1) |
| Lane report `task-AL-COMB-report.md` (TEMPLATES "Lane report" shape) | NOT STARTED |
| Themed commits | Partial: everything is in one WIP commit. Re-splitting it is optional. |
| Carry: P17 rebase on D2 / three cross-lane edits | More than three cross-lane edits were needed (listed below) |
| Carry: D1 minor (two pytests pin 13 themes) | Not touched: AL-COMB adds no theme |

## Where I stopped and next steps (in order)

1. I was choosing where the `rules:` block goes (open question 1). Findings so far:
   - At base, `scripts/research_spec.py:51` `TEMPLATE_KEYS` is strict. It refuses a top-level `rules` key in a
     template.
   - E2 (pool-17, commit `7f3bd88c`, `wave_kinds.py`) puts `rules: [{name, params}]` in the **wave manifest v2**, next
     to `rule_cell`. Its own fixture descriptions say the fitter flag "comes from the IC exe's rule table, never from
     this template".
   - My planned layout follows E2. Templates are plain `atx.research-cycle-template/v1`, with no `rules` key and no
     raw `change.flags` for the rule:
     - The rule is named in the rule registration file at `scripts/specs/p9/rules/<id>.json`, as a ready-to-paste
       `rules: [{"name": id, "params": {...}}]` plus `rule_cell` K-P9-11 keys.
     - Before writing a template, read E2's v2 fixtures in pool-17 (`git show 7f3bd88c`) and copy their template
       shape.
2. Write `scripts/specs/p9/rules/theme-cov-hedge-v1.json` and `scripts/specs/p9/rules/mom-volman-v1.json`. Put them in
   `rules/`, not `candidates/`: `wave_queue.py` validates the candidates dir as `atx.wave-candidate/v1` files, which
   need dsl, theme and tier.
   - Contents: status `proposed` with history, `source_sample_end`, a one-line `predicted_mechanism`,
     `data_class: "H"`, and the constants, each with why it is fixed.
   - theme-cov-hedge-v1 constants:
     - lookback 252: the house one-year window.
     - lag 2: the decision -> fill -> return clock.
     - step 21: the house monthly re-estimation.
     - min_pairs 126: price-risk-v1's beta minimum, half the window.
     - Hypothesis: DMRS 2020 RFS 33(5). `source_sample_end`: 2017, flagged "[verify]" (I could not confirm the paper's
       sample end).
     - Predictions: book vol ratio < 1, gross alpha ratio about 1.
   - mom-volman-v1 constants:
     - lookback 126: Barroso and Santa-Clara's six months.
     - lag 2.
     - step 21.
     - No cap (a stated choice; list it as a concern).
     - Literature: Barroso and Santa-Clara 2015 JFE 116(1) sample ends 2011; Cederburg et al. 2020 JFE 138(1) sample
       ends about 2017. Use 2017 with a basis note.
     - `requires`: the PM reads Y-2 first (DEC-16).
3. Write the two templates (`nominal_parent` x-theme-erc; outputs renamed). `requires` lists the open preconditions:
   the PM registers P9-H / P9-M, root runs the Q5 diagnostic before P9-H, and for P9-M the Y-2 read (DEC-16).
4. Write the Q5 diagnostic spec `scripts/specs/p9/diagnostics/al-comb-q5-style-variance.json` (zero trials). Plan:
   - Method: G-2a `variance_split` and `style_share_of_summed_variance` on X-5's NAV holdings, via
     `book_diagnostics.py book-csv`, then the risk verb `--book-weights --emit-exposures all` (atx-risk-v1.1). Either
     risk store is allowed (K-P9-12).
   - Primary print: the share on size, residual_vol and liquidity. Per registry text, no theme names these.
   - beta and leverage are named by low_risk's QMJ safety. The lit's 5-style list is printed as an upper bound.
   - Decision rule: a small style share argues against registering P9-H.
5. Write the lane report and commit it with `git add -f`. Then hand back (at most 15 lines).

## Every place that depends on a D2 interface (P17 rebase)

- `atx-impl/src/strategy_ic_rules.cpp`:
  - The rule table rows: tsmom's `.parse` changed to `&composition_schedule_rules`; a new volman row follows the tsmom
    row; a new hedge row comes last.
  - The `*_excludes` arrays (`resid_ids` removed).
  - The `volman_schema` / `hedge_schema` literals.
  - `stage_name` case `hedge`.
  - If D2 relocates the block parses or re-pins `params_schema` (D1-PSCHEMA), re-apply these rows on D2's form.
- `atx-impl/src/strategy_ic_rules.hpp`: the declarations of `composition_schedule_rules` and `composition_hedge`.
- `atx-impl/src/strategy_ic_mom_volman.cpp`:
  - `composition_schedule_rules` dispatches to D1/v8 `composition_schedule` (defined in
    `strategy_ic_theme_tsmom.cpp`). Follow it if D2 renames or moves it.
  - It reads `PinnedWeights` fields `std_themes`, `std_theme_count`, `values`, `residualise`, `schedule`,
    `schedule_from`, `schedule_mass`, `schedule_off`.
- `atx-impl/src/strategy_ic_theme_hedge.cpp`: reads `PinnedWeights.std_themes / residualise / sleeves`, plus
  `running_rules` and `CompositionRule::stage`.
- `atx-impl/src/strategy_ic_composition.{hpp,cpp}`: `IcStageKind::hedge` appended to the enum, `StagePlan.hedge`,
  `set_theme_hedge`, the `finish` branch `else if (p.hedge)`, and `ic_composition_stage_bytes`.
- `atx-impl/src/strategy_ic_runner.cpp`: the include plus one line after `composition.emplace(...)`:
  `ATX_TRY_VOID(attach_theme_hedge(pinned,role,*composition));`.
- `atx-impl/tests/fixtures/composition_rules_list.json` (K-P9-6):
  - Two rows added: volman after tsmom, hedge at the end.
  - The resid / tsmom / two-speed `incompatible` lists changed.
  - It was regenerated with a scratch script that reads the schema literals from rules.cpp and reproduces
    `recipe_sha256` (verified on 4 existing rows). If D2 re-pins the fixture, regenerate it from the merged C++ rather
    than hand-merging. root's `--list-rules --json` must equal the fixture (JSON-equal).
- `atx-impl/tests/strategy_ic_rules_test.cpp` `TableCoversEveryRule`: ids / stages / keys lists, block list, resid
  `incompatible` size 2 -> 4, and the `requires_any` loop.
- `atx-impl/tools/fit_composition_weights.py`:
  - Two imports.
  - `PARENT_CHECKED_MODIFIERS` gains `composition_mom_volman, composition_theme_hedge` at the tail.
  - Two attach blocks after the two_speed block (around lines 2578-2586).
- `atx-impl/tools/composition_mom_volman.py` uses `composition_theme_tsmom.sleeves` and `.SERIES`. If D2 deletes the
  Python tsmom mirror (G-P5), the volman sleeve must come from wherever D2 puts it.
- `atx-impl/tools/test_fit_composition_rule_plugins.py`: the `MODIFIER_MODULES` pin, the `written` dict and the
  absent-flag loop.
- CMake tails: `atx-impl/CMakeLists.txt` (one block: two sources plus the Debug /O2 flags, as for rules.cpp) and
  `atx-impl/tests/CMakeLists.txt` (one `target_sources(atx-impl-strategy-ic-tests ...)` block). Keep both sides on
  conflict.

## Files touched

- Owned (new):
  - Kernel and its test:
    - `atx-engine/include/atx/engine/combine/char_cov_hedge.hpp`
    - `atx-engine/tests/combine/combine_char_cov_hedge_test.cpp` (auto-globbed into `atx-engine-combine-tests`)
  - Hedge rule and its test:
    - `atx-impl/src/strategy_ic_theme_hedge.{hpp,cpp}`
    - `atx-impl/tests/strategy_ic_theme_hedge_test.cpp`
  - Volman rule and its test:
    - `atx-impl/src/strategy_ic_mom_volman.{hpp,cpp}`
    - `atx-impl/tests/strategy_ic_mom_volman_test.cpp`
  - Fitter plugins and their tests:
    - `atx-impl/tools/composition_theme_hedge.py`, `atx-impl/tools/composition_mom_volman.py`
    - `atx-impl/tools/test_composition_theme_hedge.py`, `atx-impl/tools/test_composition_mom_volman.py`
- Cross-lane: all files in the D2 list above.
  - That is more than P17's three (row, plugin entry, schedule hook). The hedge needs the role's returns inside the
    composition, so it also needs the composition stage, the runner hook and the fixture/test pins.
  - Report this.

## Tests and last results

- pytest (explicit paths, from `atx-impl/tools`), all exit 0:
  - New: `test_composition_theme_hedge.py test_composition_mom_volman.py` -> 17 passed.
  - Existing:
    - `test_fit_composition_rule_plugins.py`, `test_composition_theme_tsmom.py`, `test_composition_two_speed.py`,
      `test_composition_rules.py`, `test_composition_theme_erc.py`, `test_composition_resid.py`,
      `test_composition_ic_shrink.py` -> 90 passed, 17 subtests.
    - `test_fit_composition_weights.py`, `test_fit_composition_weights_pool.py`,
      `test_fit_composition_weights_store.py` -> 141 passed.
    - From the pool root: `scripts/tests/test_no_python_mirror.py scripts/tests/test_no_versioned_scripts.py` ->
      11 passed.
  - These ran before the final C++ formatting pass. No Python changed after them.
- Gtests (written, never compiled or run):
  - `CharCovHedge.{ClosedForm2Names, ClosedFormFiveNames, WindowSumsAreTheTrailingWindowOls,
    FutureReturnDoesNotChangePast, RefusesBadInputAndCountsScratch}`
  - `ThemeHedgeRule.{RegisteredConstantsAndRow, FlagAbsentIdentity, BlockParseAndRefusals,
    BeforeTheFirstEstimateTheHedgeIsTheParent, AddThemeHedgedIsTheMassTimesTheKernel, StageWiringAndRefusals,
    SessionReturnsAreTheGuardedIntervalReturns}`
  - `MomVolman.{RegisteredConstants, StepVarianceClosedForm, ScheduleFromTheWeightsFile, SharedParseAndRefusals}`
  - The closed forms were cross-checked with a scratch Python mirror of the kernel (not committed).
- Root verification:
  - Build `atx-equity-strategy-ic`, `atx-impl-strategy-ic-tests`, `atx-engine-combine-tests`, `atx-impl-tests`.
  - Gtest filter
    `CharCovHedge.*:ThemeHedgeRule.*:MomVolman.*:CompositionRules.*:StrategyIcComposition.*:ThemeTsmom*:TwoSpeed*:ThemeResid*:StrategyIcRunner.*`.
  - `--list-rules --json` must be JSON-equal to the fixture.
  - X-5 `u` / `w` / fit outputs must be byte-identical with both flags absent. `script_sha256` of the fitter changes;
    the producer fingerprints are unchanged (FACTOR 962cd507..., CONTEXT 70e1eda5..., AIM f1fb0d3a..., AIM_ERA
    9a880f97..., checked against `git show 1239a5ff`).

## Decisions (and why)

- The hedge is a new composition stage `IcStageKind::hedge`, appended last. Its returns are handed in by
  `set_theme_hedge`. finish replaces the standardise re-rank add with `add_theme_hedged`, which walks schedule blocks
  as finish does. Before the first hedge-ratio estimate (decision 253), the output is the parent's bit for bit.
- The hedge excludes sleeves (two-speed) and theme-resid; the brief defines the hedge on the standardised blend. It
  stacks with theme-tsmom or mom-volman schedules.
- mom-volman shares the `theme_schedule` block and the tsmom row's parse. The dispatcher is
  `composition_schedule_rules`, which sends `rule == mom-volman-v1` to the volman parse and everything else to v8's
  unchanged parse. So volman and tsmom exclude each other.
- The fitter records the sleeve exactly as tsmom's step 1 (`composition_theme_tsmom.sleeves`). The C++ computes
  sigma, target and scale (G-P5 / PM8-12).
- The hedge fitter writes only the constant spec block. Everything is estimated at apply time from the role's own
  guarded session returns (`session_interval_returns`, price-risk-v1's guard).
- The v8 refusal messages are kept verbatim. New refusals are separate messages.
- The K-P9-9 entry uses `name` (not `id`), as K-P9-9 states.

## Open questions for the PM

1. The brief says the templates carry `rules:` blocks. At base `TEMPLATE_KEYS` refuses that key. E2's v2 design puts
   `rules:` in the wave manifest. Should I follow E2 (my plan, with `rules` in the registration files and later
   manifests) or extend `TEMPLATE_KEYS` (a cross-lane edit on E2's area)?
2. Is it OK that the cross-lane edits exceed P17's three (the list above)?
3. Is it acceptable to edit D1's test pins and fixture for new rows (expected-value additions, not changes)?

## Traps for the next agent

- The shell hook rewrites `grep` to `rg` and compacts `git diff` output. Use the Grep tool, or write diffs to a file
  and Read it. Heredocs with complex quoting fail: write Python scripts to the scratchpad and run them.
- Line endings:
  - Edited existing files are CRLF in the working tree; keep CRLF when rewriting them (read bytes, replace, write
    back).
  - New C++ and Python files are LF.
  - The fixture JSON is LF.
- Do not run clang-format (house rule). Hold 100 columns by hand. All new and edited C++ lines are currently
  <= 100 columns.
- The fitter e2e tests need weighted `price_momentum` members. The synthetic world's `medium` and `medium_half` are
  re-themed to price_momentum in the two new test files.
- `wave_queue.py` validates every file in a candidates dir. Do not put rule files there.
- The fitter suites take about 3 minutes. Run them in the background with explicit paths only (PY-HYG).
