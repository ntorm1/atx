# Task FIX-4a report: the theme-resid rule and the pooled fit

Lane FIX-4a (pool 4), branch `feat/platform-v8-fix4a-20261001`, base `43a0447d`. Rulings PM4-7, PM4-10, PM4-11, PM4-12.
No C++ was built (lane rule). No real data was read. Every C++ line in this lane compiles for the first time at
integration under `/W4 /permissive- /WX`.

## Commits

| finding | commit(s) | kind |
|---|---|---|
| R6B-O-1 | `d9726cfa` | tests (the code change was the integrator's `393910ed`, kept) |
| R6B-O-2 (PM4-11) | `e8997d64` | Python |
| R6B-O-3 (PM4-12) | `72696785` | C++ + Python |
| R6B-O-4 | `263077f8` | C++ |
| R6B-O-5 | `b8b68f4e` (PM's WIP at the owner stop) + `aeda5bd7` (review fixes) | Python + scripts |
| R6B-O-6 | `d928c299` | Python test |
| R6B-O-7 | `c3110301` | C++ tests + Python test |
| R6B-C-1 (PM4-7) | `233accd9` | Python |
| R6B-C-5 | `2a47da99` | C++ |

## What root builds and runs

C++ (all findings that touch it: O-1, O-3, O-4, O-7, C-5):

```powershell
powershell scripts\atx-build.ps1 check atx-impl\src\strategy_ic_admission.cpp
powershell scripts\atx-build.ps1 check atx-impl\src\strategy_ic_theme_resid.cpp
powershell scripts\atx-build.ps1 check atx-impl\src\strategy_ic_runner.cpp
powershell scripts\atx-build.ps1 build atx-impl-strategy-ic-tests atx-equity-strategy-ic
powershell scripts\atx-build.ps1 -Ctest -R "atx-impl-strategy-ic-tests\.(ThemeResid|ThemeResidRunner|CompositionV8|StrategyIcRunner|GroupResidualise)\."
```

(`atx-impl-tests` also globs both test files; it need not be built for this lane.)

Python, every suite the lane touched (all green at `2a47da99`):

```text
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_composition_resid.py atx-impl/tools/test_fit_composition_weights.py atx-impl/tools/test_fit_composition_weights_pool.py atx-impl/tools/test_fit_composition_weights_store.py atx-impl/tools/test_composition_rules.py atx-impl/tools/test_composition_ic_shrink.py
  -> 192 passed, 14 subtests passed
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_research_spec.py scripts/tests/test_research_cycle.py scripts/tests/test_research_cycle_roles.py scripts/tests/test_research_cycle_label_role.py scripts/tests/test_cycle_e2e.py scripts/tests/test_cycle_resume.py
  -> 169 passed, 4 skipped (RESEARCH_CYCLE_LIVE_ROOT x3 and ATX_EQUITY_BIN x1: live-data / binary gated)
```

Identity (flag absent), how root verifies after the build:
- IC runner: re-run any landed weighted IC pass with the new `atx-equity-strategy-ic` (exact argv of its
  `receipt.json` `command`, only `--output` new; the R-1 w pass when it exists, else the v7.1 weighted pass). Expected
  byte-identical `recipe.json`, `train_combined.{f64,json}`, `train_combined_member.u8`, `train_combined_finite.u8`,
  `train_planned_targets.csv`, `train_daily_ic.csv`. Reason: O-4's recipe/manifest keys exist only with a
  `theme_residualise` block; O-3's tie step runs only under that block; C-5 adds no key and every file the fitter writes
  passes its check.
- Fitter: the parent's fit argv on this build gives the same `composition_weights.json` and `admission.json` except
  `provenance.script_sha256` / `inputs.script_sha256` (and the admission SHA they move). `--theme-resid` absent:
  `composition_resid.apply` returns before touching anything. `--era` with a composition that was pooled before C-1:
  the same code path (only the whitelist grew).

## Per finding

### R6B-O-1: R-11 on every rerank-true rule of the table (`d9726cfa`)
- Kept the integrator's `393910ed` (`STANDARDISE_RULES` = ew-theme-std-v1 + both ic-shrink ids; `composition_resid.apply`
  after both attach calls in `fit_prior`). Added the missing pins.
- Files: `atx-impl/tests/strategy_ic_runner_test.cpp` (`ThemeResidRunner.RidesOnEveryRerankTrueRuleOfTheTable`: the plan
  admits `theme_residualise` on ew-theme-std-v1, ic-shrink-v1 and ic-shrink-aim-v1 blocks before any payload and records
  both rules), `atx-impl/tools/test_composition_resid.py` (`FitterEndToEnd.test_an_ic_shrink_parent_is_a_standardised_parent`:
  block, order, `provenance.resid.parent_composition` = parent rule, aim provenance kept, file minus block and
  `provenance.resid` = plain parent file byte for byte).
- Verify: gtest `ThemeResidRunner.RidesOnEveryRerankTrueRuleOfTheTable`; pytest `test_composition_resid.py`.
- Deviations / cross-lane edits / risks: none.

### R6B-O-2: theme order, Ruling PM4-11 (`e8997d64`)
- `composition_resid.FROZEN_PREFIX` (the ten, in registered order) and `registered_order(prior_themes)`: the fitter's
  `PRIOR_THEMES` must start with the frozen ten, no repeat, later themes kept in list (registration) order; `theme_order`,
  `resid_block`, `attach`, `apply` take that list; `provenance.resid` records `registered_order` and `frozen_prefix`. The
  fitter passes `PRIOR_THEMES`. Never derived from a data statistic.
- Files: `atx-impl/tools/composition_resid.py`, `atx-impl/tools/fit_composition_weights.py` (1 line),
  `atx-impl/tools/test_composition_resid.py`.
- Verify: pytest `test_composition_resid.py` (`DeclaredRule.*`, `test_the_fitter_derives_the_order_from_prior_themes`).
- Risk: when E7 (LIB2) appends `filing_events` to the registry and `PRIOR_THEMES`, the pins stay green (registry ==
  `PRIOR_THEMES`; C++ list = frozen ten + `filing_events`). A theme beyond `filing_events` needs the C++ copy extended;
  `DeclaredRule.test_declared_constants_and_their_cpp_pins` fails loudly until it is.

### R6B-O-3: ties, Ruling PM4-12 (`72696785`)
- Files: `atx-impl/src/strategy_ic_theme_resid.{hpp,cpp}` (`mean_over_tie_blocks`, called in `add_theme` after
  `residualise_in_place` and the spanned test, before the re-rank), `atx-impl/tests/strategy_ic_theme_resid_test.cpp`,
  `atx-impl/tools/composition_resid.py` (`tie_block_means`; `add_session`; `kernel`), `atx-impl/tools/test_composition_resid.py`,
  `scripts/specs/v8/r11.json` (description), `task-R-11-report.md` (dated amendment).
- How a tie block is detected (both sides, the same expression in the same order): theme t's support (names with a
  present member, ascending) is sorted by the pair (z_t, support position); a tie block is a maximal run of exactly equal
  z_t (IEEE `==` on the standardised composite; z_t is the centred tied rank of the raw plane, so equal z_t iff equal raw
  sums). C++: `row` = (own[offset+support[k]], k), `std::sort`, run while `row[end].first == row[b].first`. Python:
  `sorted(range(n), key=(float(z[k]), k))`, run while `z[order[end]] == z[order[b]]`. Inside a block of two or more names
  the residual becomes sum / size, the sum started at the block's first name and continued in ascending name order; the
  block means are then re-ranked (centred tied rank) as before.
- No-tie bit identity:
  - C++ kernel: with no two equal z_t every run has length 1, the `if (end - b > 1U)` branch never runs, and
    `s.dependent` (the residual) is never written; `row` is then cleared and refilled with (dependent[k], support[k]) and
    re-ranked exactly as before PM4-12. The added sort only reads `row` scratch (capacity reserved: no allocation). The
    spanned decision precedes the tie step; the first theme returns before it.
  - Python (`composition_resid`, the numpy reference of the rule; the fitter itself writes only `{rule, order}` and never
    computes the blend): `tie_block_means` copies `e` and writes only inside blocks of two or more, so without ties it
    returns `e` bit for bit and `centred_tied_ranks` sees the same values.
  - Pinned: `ThemeResid.NoTieCompositeIsTheRegisteredRuleBitForBit` (`same_bits` against the pre-PM4-12 path on the engine
    kernels), `RunnerReference.test_no_tie_composite_is_the_registered_rule_bit_for_bit` (uint64 view equality), and
    (O-7) `ThemeResid.SmallCaseSeparatesTheRegisteredRegressors` / `test_small_case_separates_the_registered_regressors`.
  - Tied fixture: `ThemeResid.TiedCompositeStaysTiedAfterResidualisation` / `test_tied_composite_stays_tied_after_residualisation`
    (a sparse flag theme and a three-level theme; exact fractions on both sides; the no-tie-step values listed).
- Verify: gtest `ThemeResid.*`; pytest `test_composition_resid.py`.
- Deviations / cross-lane edits: none. Risk: the three-theme fixture's expected values changed on dates 0 and 1 (theme b
  ties two pairs there); Python and C++ pin the same new fractions.

### R6B-O-4: the runner checks and records `order` (`263077f8`)
- `strategy_ic_theme_resid.hpp` `theme_resid_order` (C++ copy of the registered order: frozen ten, then `filing_events`);
  `composition_residualise` refuses a weighted theme outside it and any order other than the list restricted to the
  weighted themes, before any payload; `PinnedWeights::residualise_order`; `method_recipe` / `save_combined_artifact` /
  `score_role` take the order (a span; empty = no block) and record `composition_residualise_order` beside
  `composition_residualise` in `recipe.json` and both combined manifests; `--help` text.
- Files: `atx-impl/src/strategy_ic_theme_resid.{hpp,cpp}`, `atx-impl/src/strategy_ic_admission.cpp`,
  `atx-impl/src/strategy_ic_detail.hpp`, `atx-impl/src/strategy_ic_runner.cpp`, `atx-impl/tests/strategy_ic_runner_test.cpp`,
  `atx-impl/tools/test_composition_resid.py` (C++ list pinned against `PRIOR_THEMES`), `task-R-11-report.md`.
- Verify: gtest `ThemeResidRunner.*` (+ `CompositionV8.*`, `StrategyIcRunner.*` for flag-absent bytes); pytest
  `test_composition_resid.py`.
- Risk: the `method_recipe` / `save_combined_artifact` signature changed again (bool -> span); every call site in src
  and tests was updated in the commit (re-checked: `run_ic` passes `pinned.residualise_order`, `score_role` passes
  `resid_order` or an empty span).

### R6B-O-5: R-11's re-fit checked against the parent cell (`b8b68f4e` WIP + `aeda5bd7`)
- Reviewed every hunk of the WIP against the finding and brief item 5; kept it, with two fixes in `aeda5bd7`: the
  comparison is key-presence strict (`_differing`: a key in one file only, even as null, differs; the WIP's `.get()`
  treated absent and null as equal), and `--theme-resid-parent` is refused with a pooled (era) fit by name.
- As coded: `fit_composition_weights.py --theme-resid theme-resid-v1 --theme-resid-parent PATH --theme-resid-parent-sha256
  PIN` (`load_resid_parent`, before anything is computed) pins the parent cell's `composition_weights.json` and the
  `admission.json` beside it (pinned by that file's `provenance.admission_sha256`). `composition_resid.parent_check`:
  the re-fit minus `theme_residualise` and `provenance.resid` must equal the parent file apart from
  `provenance.script_sha256` and `provenance.admission_sha256`, and the two admission tables must be equal apart from
  `inputs.script_sha256`; a parent that already carries the block is refused; differing keys are named. Recorded:
  `provenance.resid.parent_weights_sha256`, `provenance.resid.parent_check`, summary `theme_residualise_parent_sha256`.
- r11.json "passes them from the parent cell": `research_spec` derives `inputs.reference_resid_parent` = the parent's
  `fit.output/composition_weights.json` for the template whose own fit flags add `--theme-resid` (pinned by
  `lock --write` into the template's `locked`, like every derived input); `research_cycle`'s single-window fit step
  passes it as `--theme-resid-parent PATH --theme-resid-parent-sha256 PIN` and binds it in the runner receipt.
- Files: `atx-impl/tools/composition_resid.py`, `atx-impl/tools/fit_composition_weights.py`,
  `atx-impl/tools/test_composition_resid.py`, `scripts/research_spec.py`, `scripts/research_cycle.py`,
  `scripts/specs/v8/r11.json` (description only), `scripts/tests/test_research_spec.py`.
- Verify: pytest `test_composition_resid.py` (`test_the_re_fit_is_checked_against_the_parent_cells_file`,
  `test_a_re_fit_that_is_not_the_parent_cells_file_is_refused`: another composition, tampered weights, an admission
  changed beyond the script SHA, an extra null key, a resid parent, a wrong pin, a pooled fit, half flags, no flag),
  `scripts/tests/test_research_spec.py` (`test_r11_checks_its_re_fit_against_the_parent_cells_weights`,
  `test_templates_differ_from_the_parent_only_by_the_registered_change`), `scripts/tests/test_research_cycle.py`.
- Deviations:
  1. The flags are optional at the fitter; the R-11 template's single-window fit always passes them. A pooled (era)
     fit refuses them, and a descendant of R-11 (NAV-only child, R-12 add-alpha child) never gets them (the input is
     derived only for the template that adds `--theme-resid`; add-alpha re-derives every `reference_` input). Those are
     re-fits of other inputs, not of the parent file.
  2. Besides `script_sha256`, `provenance.admission_sha256` is tolerated only together with the admission-table check,
     because `admission.json` records the fitter SHA, so a changed fitter file always moves it.
- Cross-lane edits (FIX-4b owns `validate_v8_keys` and its tests; untouched): `scripts/research_cycle.py` (`INPUT_KEYS`
  gains `reference_resid_parent` with a comment; `fit_step` adds the two flags and the bind, single window only);
  `scripts/research_spec.py` (`DERIVED`, `RESID_FLAG`, `parent_references(..., resid)`, `resolve`, module doc);
  `scripts/tests/test_research_spec.py` (`NULL_PINS["r11.json"]`, the template-diff expectation, one new test).
- Open risks: module SHAs inside the parent file (`provenance.std.module_sha256` of `composition_rules.py`,
  `provenance.ic_shrink.module_sha256` of `composition_ic_shrink.py`) are compared strictly, as the finding asks: if
  either module changes between the parent's fit and R-11's fit, R-11's fit is refused (a fitter-affecting change; the
  parent must be re-fitted on the same build, or a ruling). This lane does not touch either module. The receipt binds
  only the weights file; the admission file is pinned transitively by its `admission_sha256`.

### R6B-O-6: aim parent end to end (`d928c299`)
- `FitterEndToEnd.test_an_aim_parent_keeps_its_gains_and_records_its_rule`: fits ew-theme-std-aim-v1 and the same argv
  with `--theme-resid`; asserts the rerank-true ew-theme-std-v1 block, the block `{rule, order: [value,
  reversal_seasonality]}`, `parent_composition == "ew-theme-std-aim-v1"`, `provenance.aim` present, weights differ from
  ew-theme-std-v1's (the gains), and the file minus block and `provenance.resid` is the aim parent file byte for byte.
- Files: `atx-impl/tools/test_composition_resid.py`. Verify: pytest `test_composition_resid.py`. Deviations, risks: none.

### R6B-O-7: tests that separate the registered regressors (`c3110301`)
- Kernel: `ThemeResid.SmallCaseSeparatesTheRegisteredRegressors` (one date, five names, three themes without a tie,
  W = .3/.45/.25; theme 1 absent on name 4, theme 2 on name 2): exact values -1/24, -7/20, 3/40, 17/40, -13/120. Wrong
  rules (exact): (a) residual regressors and (b) re-ranked residual regressors both -1/8, -4/15, 3/40, 41/120, -1/40;
  (c) no intercept -1/8, -4/15, 3/40, 17/40, -13/120; each >= 1/12 away. Also `same_bits` against the pre-PM4-12 path.
- Runner: `ThemeResidRunner.ThreeThemeCycleIsTheRegisteredRule`: library + `resid_a = 1 / (volume - 100000000)`
  (absent on name 0), `resid_b = (volume - 150000000) * (volume - 600000000)`, `resid_c = (volume - 50000000) * (volume -
  400000000)`, weights .3/.45/.25, themes earnings_momentum/price_momentum/value: first-appearance indices 0, 1, 2 at
  registered positions 1, 2, 0 (a 3-cycle). Every combined row d >= 63 of both roles equals -3/35, 1/20, -2/5, -13/140,
  3/140, 19/140, 1/4, 17/140 (1e-15); recipe and manifests record the order. Wrong rules (exact): (d) inverted map 1/140,
  -1/140, -51/140, 0, 3/70, 1/20, 13/140, 5/28; (a), (b) 6/35, -1/70, -13/28, -1/35, 3/140, 19/140, 13/70, -1/140; (c)
  -3/35, 1/10, -7/20, -13/140, 3/140, 19/140, 1/5, 1/14; each >= 1/20 away.
- Python mirror: `RunnerReference.test_small_case_separates_the_registered_regressors`,
  `test_three_theme_cycle_is_the_registered_rule`; `plausible_wrong_rule` implements (a)-(c) on the reference's
  functions and the inverted map runs through `blend`; all wrong-rule values asserted against their exact fractions.
- All fixtures computed in rational arithmetic (scratch implementation independent of both codes) and against the
  numpy `lstsq` reference; every residual order has relative gap >= 0.0049 (registered and each wrong rule), so rounding
  cannot reorder a rank.
- Files: `atx-impl/tests/strategy_ic_theme_resid_test.cpp`, `atx-impl/tests/strategy_ic_runner_test.cpp`,
  `atx-impl/tools/test_composition_resid.py`. Verify: gtest
  `ThemeResid.SmallCaseSeparatesTheRegisteredRegressors:ThemeResidRunner.ThreeThemeCycleIsTheRegisteredRule`; pytest
  `test_composition_resid.py`.
- Deviation: on the five-name kernel case (a) and (b) coincide (theme 2 has one residual degree of freedom); both are
  separated from the rule. The runner case separates all four.
- Risk: the runner case needs the VM to give a non-finite value for `1 / 0` (raw IEEE, vm.hpp contract) and the
  composition to treat it as absent (`std::isfinite`; the cache canonicalises to NaN). Not compiled.

### R6B-C-1: pooled fit, Ruling PM4-7 (`233accd9`)
- `POOLED_COMPOSITIONS` gains `ic-shrink-v1` and `ic-shrink-aim-v1`: the single-window branch
  (`composition_ic_shrink.ic_shrink` on the admission rows' `train_mean`, the variant with the aim gains) runs on the
  pooled admission and the pooled aim records (`ensure_records(aim=...)`, `pool_aims`, as ew-theme-std-aim-v1 already
  did). `POOLED_THEME_RESID = ("theme-resid-v1",)`: `--theme-resid` under `--era` runs `composition_resid.apply` on the
  pooled document unchanged; any other `--theme-resid` id, like any other `--composition`, is refused by name before
  anything is read. Docstrings updated.
- Files: `atx-impl/tools/fit_composition_weights.py`, `atx-impl/tools/test_fit_composition_weights_pool.py`.
- Verify: pytest `test_fit_composition_weights_pool.py`: one-era equality (pool keys removed, byte for byte) for
  `ic-shrink-v1`, `ic-shrink-aim-v1` (`test_pooled_ic_shrink_fit_over_one_era_equals_the_single_window_fit`) and
  theme-resid-v1 on each of its four parents (`test_pooled_theme_resid_over_one_era_equals_the_single_window_fit`,
  pooled file minus block = pooled parent file); two history eras against `composition_ic_shrink.ic_shrink` on the pooled
  admission (`test_two_history_eras_pooled_ic_shrink_is_the_rule_on_the_pooled_admission`, era files equal);
  refusals by name. `test_fit_composition_weights.py` (constant relations) green.
- Deviations / cross-lane edits: none.
- PM4-10 (no code in this lane): an infeasible 1/(2T) cap in the pooled ic-shrink fit is refused exactly as in the
  single window, by `composition_rules.member_cap` ("member cap 1/(2T)=<c>: no other theme can take the excess of theme
  <t>"), exit 1 before anything is published; under PM4-10 that cell is undefined (adds 0), not a rejected trial. The
  wording was left unchanged: `composition_rules.py` is not this lane's file and its `module_sha256` is part of every
  std/std-aim parent file that O-5 compares strictly. If root wants the ruling named in the message, that edit belongs
  before any v8 parent cell is fitted.

### R6B-C-5: the recorded rule must write the block (`2a47da99`)
- `strategy_ic_admission.cpp`: each row of the `theme_standardise` rule table names `also_written_by` (ew-theme-std-v1:
  R-3's `ew-theme-std-aim-v1`; others none) and `identity_source` (ew-theme-std-v1: `ew-theme-v1`, the R-1 rerank-off
  identity device of `composition_rules.identity_document`; others none). `composition_recorded_rule`, called in
  `composition_weights` right after `composition_standardise` and before any payload: with a string `provenance.rule`,
  (1) a rule that writes a row must carry exactly that row's block ("record provenance.rule X, which writes
  theme_standardise rule W, but carry ..."); (2) a block whose recorded rule writes no row is refused unless it is the
  row's rerank-off identity device on `identity_source` ("carry theme_standardise rule B, which their provenance.rule X
  does not write"). Files without `provenance.rule` are not checked.
- Files: `atx-impl/src/strategy_ic_admission.cpp`, `atx-impl/tests/strategy_ic_runner_test.cpp`
  (`CompositionV8.RecordedRuleMustWriteTheStandardiseBlockBeforeAnyPayloadOrOutput`: 7 admitted files, 11 refused,
  plan-only and full run, empty log, no output dir).
- Verify: gtest `CompositionV8.*:ThemeResidRunner.*:StrategyIcRunner.*`.
- Deviation: the finding's smallest fix covers an ic-shrink `provenance.rule`; the brief asks for every row, so both
  directions are checked for all three rows, with the identity device admitted explicitly.
- Risk: a producer other than the fitter that records `provenance.rule` and grafts a `theme_standardise` block would now
  be refused; a grep of the tree finds only `composition_rules` (attach_std, identity_document) and
  `composition_ic_shrink.attach`, which pass. The marginal verb's reader (`ic_weights_themes`) is unchanged.

## C++ compile self-review (lane C++: O-3, O-4 by the earlier session; O-7, C-5 here)
- `strategy_ic_admission.cpp`: the aggregate rows initialise every member (`{}` for empty `string_view`s: no
  `-Wmissing-field-initializers`); the two `constexpr std::string_view` constants are used; `std::string ==
  std::string_view` is the C++20 comparison; `get_ref<const std::string&>` is the file's idiom; the ternary operands are
  both `std::string`; the function sits in the existing unnamed namespace after `standardise_row`.
- Tests: `for (const auto& [id,dsl]:{std::pair{...},...})` is the file's existing idiom (CTAD to
  `pair<const char*,const char*>`); lambdas with default arguments; `d["provenance"]["rule"]=recorded` (no
  single-pair initializer-list ambiguity); nested `vector<vector<f64>>` brace init with int literals and `kNaN` as in
  the existing tie test; lines within the file's 120-column practice; new suite/test names unique.

## Open risks (summary)
1. C++ not compiled (lane rule): O-3, O-4, O-7, C-5.
2. O-5 compares module SHAs of `composition_rules.py` / `composition_ic_shrink.py` strictly (see O-5).
3. O-2: a theme registered after `filing_events` needs the C++ copy extended (pin test fails loudly).
4. Cross-lane: `scripts/research_cycle.py`, `scripts/research_spec.py`, `scripts/tests/test_research_spec.py` (O-5);
   FIX-4b edits `validate_v8_keys` and its own tests in the same two files: textual merge only expected.
