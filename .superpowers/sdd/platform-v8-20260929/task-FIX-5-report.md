# Task FIX-5 report: Python fix round after review 6C (R6C-1..R6C-6, Ruling PM5-12)

Lane FIX-5, worktree `C:/atx-wt/pool-10`, branch `feat/platform-v8-fix5-20261001`, base `41ef00fb` (root's code head with
FIX-4a and FIX-4b merged). Python and text only: no C++ file touched, nothing built, no real data opened, nothing dated
2024-01-01 or later read. Binding: review-6c-fix4.md rows R6C-1..R6C-6, Rulings PM5-11 and PM5-12.

## Commits (one per finding, in the brief's order)

| finding | commit | files | verified by |
|---|---|---|---|
| R6C-1 | `c8f92975` | `atx-impl/tools/mega_report/v8.py`, `test_mega_report_v8.py`, `test_mega_report_v8_render.py` | `test_an_accept_whose_rule_is_na_is_refused_naming_the_part`, `test_r8_accepted_with_its_band_unread_is_refused_naming_the_year` (test_mega_report_v8.py); `test_accepted_optional_cells_whose_parent_row_is_missing_are_refused` (render) |
| R6C-2 | `e9ca4e29` | `docs/plans/mega-alpha-v8-pitch.config.json`, `docs/plans/mega-alpha-scorecard-v8.template.md`, `scripts/specs/v8/r1-comp-v8.json`, `r10.json`, `r11.json`, `scripts/tests/test_research_spec.py`, `test_mega_report_v8_render.py` | `test_r11_appends_theme_resid_to_the_parents_fit` (test_research_spec.py); `test_committed_criteria_carry_every_registered_part`, `test_scorecard_template_carries_every_registered_part`, `test_v8_pitch_without_the_optional_cells_renders_the_pre_pm4_8_bytes` (render) |
| R6C-5 | `43ac15fa` | `v8.py`, `test_mega_report_v8.py`, `test_mega_report_v8_render.py` | `test_n_after_is_counted_from_the_cell_states` (render), `test_cell_states_follow_the_recorded_branch` (open flag) |
| R6C-6 | `f8adf49d` | `v8.py`, `test_mega_report_v8.py` | `test_an_own_undefined_verdict_on_a_cell_that_ran_is_refused` |
| R6C-4 | `9e38cdda` | `atx-impl/tools/fit_composition_weights.py`, `test_composition_resid.py` | `FitterEndToEnd.test_the_registry_themes_must_be_prior_themes_under_theme_resid`, `FitterEndToEnd.test_the_fitter_derives_the_order_from_prior_themes` |
| R6C-3 (Python half) | `ae77d274` | `atx-impl/tools/test_composition_resid.py` | `RunnerReference.test_unequal_tie_blocks_beside_singletons_pin_the_block_mean` |

Each new test was run against the pre-fix module (the finding's file restored from the commit before it) and fails
there: R6C-1 `[] == [refusal]` for R-1, R-8, R-10/R-11/R-12; R6C-5 R-12's N after 51 != 49; R6C-6 `[] == [refusal]`;
R6C-4 the fit proceeds past the order to the parent read. R6C-3 pins exact values that a block sum or rank averaging
misses by >= 2/7.

## What was built

### R6C-1 (`v8.py`): an accepted rule-bearing cell whose rule is n/a is refused, naming the part
- `criterion_eval` returns a new key `na`: for each computed check whose result is n/a on a document that was read, why
  (`<check> of the cell|parent n/a in its nav_summ row|NAV summary.json`, `... does not compare with ...`, and for a
  `source: years` check `years_na`: `its nav_summ row has no year_table (run nav_summ with --protocol v8)`,
  `TRAIN year Y is not in its year table (no return rows)`, `TRAIN year Y: ann_vol n/a (1 return rows)`). `_years_by`
  is shared by `year_values` and `years_na`.
- `ladder_rows` -> `_rule_na(v8, c, par, sm, (r, pr), mech, ce, pd, docs)` on every rule-bearing row whose rule is n/a:
  `na_parts` (the cell's nav_summ row missing, the parent's row missing when the criterion compares with it
  (`_reads_parent_row`), a mechanics metric n/a or no mechanics configured, no paired test configured, a paired dSR
  missing from its file, plus `ce['na']`) and `na_inputs` (the nav_summ JSON, a paired test file, a NAV summary.json
  whose own unavailable block already explains the n/a).
- `verdict_rule_checks` -> `_na_why`: an accepted row with rule n/a is refused as
  `recorded accepted ('X') but the rule of v8-prereg item 5 is n/a: <parts>`; the unread-manual-part message is
  unchanged (`its criterion is not read: ... (set the part's met)`), both joined with `; and ` when both apply; with
  nothing named and no unavailable input, a fallback refusal quotes the rule text. Baseline, report-only and undefined
  rows carry no rule (unchanged).

### R6C-2 (Ruling PM5-11): the statistic named, "planned" dropped
The criterion of R-1, R-10 and R-11 reads, in the pitch config, the scorecard template's Cells and ladder rows, and
the three spec descriptions: "turnover per unit gross (executed: tau_gmv_mean / mean_gross_leverage_all_rows, S2) not
higher than the parent", with PM5-11 cited. The scorecard template states the PM5-11 disclosure under 2a (executed
one-way GMV turnover per unit gross, cell against parent, `<=`; nav_summ has no planned-turnover statistic; judged on
the executed number where caps or blocked orders make the two disagree). Computed check unchanged. Sprint plan and
ledger not edited.

### R6C-5 (`v8.py`): "N after" from the cell states
- `cell_states` gains `'open'`: True while a cell's `defined_if` names a cell not yet settled.
- `n_after(cells, states)`: the first cell's configured `n` anchors the count; each later cell adds 1, an undefined
  cell 0; from the first open cell on, the configured `n` (the plan) is shown. `ladder_rows` renders it and notes a
  configured `n` that differs: `R-12: N after 49 from the cell states (an undefined cell adds 0), the configured n is
  51`.

### R6C-6 (`v8.py`): an own "undefined" verdict on a cell that ran is refused
For a cell whose own verdict records it undefined (PM4-10), `ladder_rows` stats its configured paired path (`_exists`:
the Registry's seal check, then `Path.exists()`; never read or hashed, not in the manifest) and `branch_checks`
refuses it: `recorded undefined ('...') but its paired test P exists: the cell was run, and an undefined verdict
records a cell that could not be formed (Ruling PM4-10)`.

### R6C-4 (`fit_composition_weights.py`): registry themes == PRIOR_THEMES under --theme-resid
`require_resid_order(args)`, called in `fit()` right after the prior / theme-resid pairing check and before
`load_resid_parent` (so before any input is read or anything computed): with `--theme-resid`, `prior_themes()` must be
exactly `PRIOR_THEMES` (same themes, same order), else `FitError` naming both tuples and the source:
`--theme-resid theme-resid-v1: the admissible themes (registry registry.json: ...) are not PRIOR_THEMES, the
registered theme order (Ruling PM4-11: ...); extend V7_APPENDED_THEMES to the registry's themes, in its order, before
a theme-resid fit`. Flag absent: no check. Checked on this tree: the real `atx-impl/strategies/alphas/registry.json`
themes equal `PRIOR_THEMES` in order.

### R6C-3, Python half (`test_composition_resid.py`)
A separate one-date fixture (`UNEQUAL_*`, eight names, W 1/2 each): theme 1 has tie blocks {2, 4, 6} and {5, 7}
beside singletons 0, 1, 3. The rule orders the theme's add 0 < 1 < {5,7} < {2,4,6} < 3; a block sum lifts both blocks
above name 3; the per-block mean of the residual's ranks puts {5,7} level with name 3 below {2,4,6}. Exact values of
the rule and of both wrong tie steps (computed independently in rational arithmetic) are pinned; the wrong steps are
injected by patching `composition_resid.tie_block_means` and each is >= 2/7 away. The shared PM4-12 fixture
(`TIE_PLANES`, mirrored in C++) is unchanged.

## How root verifies

```
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_mega_report_v8.py atx-impl/tools/test_mega_report_v8_render.py atx-impl/tools/test_mega_report_pitch3.py atx-impl/tools/test_mega_report_seal.py atx-impl/tools/test_mega_report_sig_corr.py atx-impl/tools/test_composition_resid.py atx-impl/tools/test_fit_composition_weights.py atx-impl/tools/test_fit_composition_weights_pool.py atx-impl/tools/test_fit_composition_weights_store.py atx-impl/tools/test_composition_ic_shrink.py atx-impl/tools/test_composition_rules.py atx-impl/tools/test_alpha_report_card.py atx-impl/tools/test_alpha_report_card_store.py atx-impl/tools/test_book_diagnostics.py atx-impl/tools/test_book_monitor.py atx-impl/tools/test_exposures_export.py atx-impl/tools/test_horizon_stats.py atx-impl/strategies/test_generate_library.py atx-engine/tools/test_research_window.py atx-engine/tools/test_research_fields_v8.py scripts/tests/test_research_spec.py scripts/tests/test_research_cycle.py scripts/tests/test_cycle_scoring.py
```
Result on `ae77d274`: RESULT_LINE (test_research_cycle's live-root tests skip without `RESEARCH_CYCLE_LIVE_ROOT`).
No build target: no C++ changed.

Identity:
- Report: `test_v7_config_renders_the_pre_v8_bytes` (v7 golden) and
  `test_v8_pitch_without_the_optional_cells_renders_the_pre_pm4_8_bytes` hold: R6C-1/5/6 move no byte of a config
  without the optional cells. The latter now sets R-1's criterion text back to its text of then
  (`R1_TEXT_PRE_PM5_11`), because PM5-11 changed the config's text (a text-only change, rendered in the ladder's
  criterion column). The committed config (all verdicts pending) still renders 13 ladder refusals
  (`test_committed_config_refuses_its_pending_verdicts`) and the configured N after values.
- Fitter, identity 4: `fit_composition_weights.py` changed, so `provenance.script_sha256` moves in every weights file
  (and the admission's `inputs.script_sha256`, hence `provenance.admission_sha256`); every other byte is unchanged with
  the flag absent (the new check returns at once without `--theme-resid`). Root re-checks identity 4 for this reason.
  The `--theme-resid-parent` check tolerates exactly these two differences (PM5-1), so a parent fitted before this
  commit still passes.

## Deviations from the brief
1. R6C-1: an n/a whose input is its own unavailable block (nav_summ JSON, paired test file, NAV summary.json missing
   or refused) is not refused a second time; that keeps the contract "one named unavailable block per missing input"
   (`test_each_missing_input_is_one_named_unavailable_block`) and is the reading of the review's fix ("the only
   refusal for an n/a rule needs crit_unread ... a note, no unavailable block, no check"). The cell is still counted
   by the CLI through that block. Beyond the brief's list the refusal also names a missing metric in a row or summary
   that was read, a mechanics metric n/a, no mechanics or no paired test configured, and a paired dSR missing; a
   fallback refusal covers any n/a left unnamed with no unavailable input. `criterion_eval` gains the key `na` (two
   exact-shape assertions updated).
2. R6C-1: the rule's logic is unchanged: a rule with one part false and another n/a stays n/a, so such an accept is
   now refused as "n/a" (before: not refused) rather than "rejects".
3. R6C-2: the texts also cite "Ruling PM5-11"; the scorecard template gains the PM5-11 disclosure sentence under 2a
   (the ruling says "disclosed in the scorecard").
4. R6C-5: computed rather than only checked. A defect cell adds 1 (its blind rerun takes its slot, v8-prereg item 7);
   while a branch is open the configured n is shown; a differing configured n is a note, not a refusal.
5. R6C-6: only a cell whose own verdict records it undefined is stat-ed; a cell undefined by its branch (E-38 / E-45)
   is not (the brief's scope).
6. R6C-4: `test_the_fitter_derives_the_order_from_prior_themes` extended PRIOR_THEMES alone (now refused); it now
   registers filing_events in a temporary registry as well, which is what "once it is registered" means.
7. R6C-3: a separate fixture rather than a third date of `TIE_PLANES`, so the C++ mirror of that fixture stays exact
   until the deferred C++ half.

## Cross-lane edits
Description text only in `scripts/specs/v8/r1-comp-v8.json` (lane R1), `r10.json` (COMB2), `r11.json` (ORTH), as the
brief names them.

## Open risks
- The spec digests (`research_spec.spec_digest`, recorded in `cycle_binding.json` / `cycle_verdict.json`) of R-1,
  R-10 and R-11, and of every template chain resting on R-1, move with the description edits. Harmless before the
  locks and before those cells run (PM5-12); a binding written earlier would read as a changed spec.
- R6C-6: a branch-undefined cell whose paired file exists (someone ran it anyway) is not refused.
- R6C-5: if the PM treats a defect cell as excluded with no rerun, later N after values are one too high until the
  defect is replaced; the configured n is no longer what the ladder shows once the branch is settled.
- R6C-1: `refused()` truncates a block's text at 320 characters (existing); a refusal naming several TRAIN years can be
  cut in the HTML (the full text is in `ladder_checks`).
- R6C-4: E7 must extend `V7_APPENDED_THEMES` (and the C++ `theme_resid_order` copy) together with the registry.
- Deferred by PM5-12 to integration 8: the C++ halves of R6C-3 (divisor, summation order in the C++ kernel test) and
  R6C-7.
- The scorecard template's "N after" column is static text the PM fills.
