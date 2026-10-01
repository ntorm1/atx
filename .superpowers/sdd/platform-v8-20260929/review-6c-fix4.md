# Review 6C: fix round FIX-4 (read-only, adversarial)

Ranges reviewed (git read commands only; full head files read where cited, not only hunks):
- FIX-4a, `C:/atx-wt/pool-4`, `43a0447d..98ef8d89` (17 files): composition_resid.py, fit_composition_weights.py (pooled
  dispatch, load_resid_parent, apply call), strategy_ic_theme_resid.{hpp,cpp}, strategy_ic_admission.cpp,
  strategy_ic_detail.hpp, strategy_ic_runner.cpp, both C++ test files, test_composition_resid.py,
  test_fit_composition_weights_pool.py, research_cycle.py / research_spec.py / r11.json hunks, test_research_spec.py
  hunk, task-R-11-report.md amendment, task-FIX-4a-report.md.
- FIX-4b, `C:/atx-wt/pool-10`, `43a0447d..0b093a4c` (11 files): mega_report/v8.py (head read 160-893, 1584-1600),
  pitch config, scorecard template, research_cycle.py `argparse_values` / `validate_e27b`, test_research_spec.py hunk,
  test_mega_report_v8.py optional-cell tests, strategy_spo_cli_fixture.hpp, strategy_spo_test.cpp hunk,
  strategy_spo_v3_test.cpp new tests, task-FIX-4b-report.md.
- Binding texts: task-FIX-4-brief.md; progress.md entries E-35, E-35a, E-36, E-38, E-43, E-44, E-45, E-27b, PM4-7..PM4-12,
  PM5-1, PM5-2 (pool 2, read only); review-w1-T.md T-10.

Not reviewed: the integrator's merged tree in pool 2 (41ef00fb, c17fb449); test_mega_report_v8_render.py (identity
pins); the engine's group_residualise.hpp MGS kernel and the spo Engine's back-fill code (taken as the 6B reviewers
left them); the three-theme fixture's new EXPECTED_BLEND values (not hand-checked); nothing compiled, run or tested.
Nothing under build-equity/ and nothing dated 2024+ was opened.

Hand checks done (exact arithmetic, by hand): the PM4-12 tie fixture (both dates, rule and no-tie-step values), the
O-7 small case (rule and wrong rule (c)), the O-7 three-theme cycle (all eight values). All match the pinned fractions.

## Findings

| id | sev | file:line | failing scenario | smallest fix |
|---|---|---|---|---|
| R6C-1 | M | `atx-impl/tools/mega_report/v8.py:800-805` (verdict_rule_checks), `:447-459` (years_eval), `:730-731` (ladder_rows) | A cell recorded ACCEPTED whose rule is n/a because a computed input is missing passes ladder_checks: R-8's nav_summ row lacks `year_table`, or one TRAIN year (or that year has <= 1 return row, `ann_vol` None) -> `years_eval` None -> `crit_met` None -> `rule` None with `crit_unread == []`; the only refusal for an n/a rule needs `crit_unread`. Same for a cell (or its parent) missing from the v8.summ JSON: a note, no unavailable block, no check. The ladder then shows R-8 (or R-10/R-11/R-12) accepted with the criterion never computed. | In `verdict_rule_checks`, refuse `k == 'accepted' and rule is None` for every non-baseline, non-report row (name the n/a part: unread manual part, missing row, missing TRAIN year), or make a missing nav_summ row / TRAIN year an unavailable block of the ladder. |
| R6C-2 | M | `docs/plans/mega-alpha-v8-pitch.config.json:140,146` (and R-1 at `:72`) | E-44 / PM4-8 register R-10 and R-11 (and R-1) on "planned turnover per unit gross not higher than the parent"; the computed check is `tau_gmv_mean / mean_gross_leverage_all_rows`, executed one-way GMV turnover (nav_summ has no planned-turnover statistic). The lane's own criterion text now says "planned" over a check that reads executed turnover; fills, caps and blocked orders can make the two disagree in sign against the parent. Same root as T-10 (review-w1-T.md:175, never ruled). | A one-line ruling naming `tau_gmv_mean` per unit gross as R-1's registered statistic (and drop "planned" from the three texts), or add planned turnover to nav_summ and point the three checks at it. |
| R6C-3 | m | `atx-impl/tests/strategy_ic_theme_resid_test.cpp:330`, `atx-impl/tools/test_composition_resid.py:294-323` | The tie fixture does not pin the block-mean divisor or rank-vs-value averaging: a block SUM instead of mean gives the same ranks on both dates (date 0: two blocks of opposite sign; date 1: three blocks of size 2), and "average the raw residual's ranks per block, then re-rank" gives -.2/.4 on date 0 and -.4/0/.4 on date 1, the rule's values exactly. The summation-order pin (1e16, 1, -1e16) exists in Python only; no C++ counterpart. | Add a date with blocks of unequal size beside singletons whose block-mean order differs from their mean-rank order (both sides), and the 1e16 summation case to the C++ kernel test. |
| R6C-4 | m | `atx-impl/tools/fit_composition_weights.py:2416` vs `:418-427` | R-11's order comes from the constant `PRIOR_THEMES`; the fitter admits members by the registry's `themes` table (`prior_themes()`). If E7 adds `filing_events` to registry.json without extending `V7_APPENDED_THEMES`, R-7 fits filing_events members and R-11 refuses ("themes outside the registered order"): R6B-O-2's scenario again. Only `test_declared_constants_and_their_cpp_pins` ties the two lists. | Under `--theme-resid`, `require(prior_themes()[0] == PRIOR_THEMES)` before anything is computed (or pass the registry tuple through `registered_order`). |
| R6C-5 | m | `docs/plans/mega-alpha-v8-pitch.config.json:137-149`, `v8.py:686-693` | "N after" is the static config `n` (R-10 49, R-11 50, R-12 51). When R-10 is undefined (E-45 / PM4-10) or R-1 is rejected, the later cells' N after is one or two lower; nothing recomputes `n` or checks it against the undefined cells (which add 0). | Compute `n` from the cell states (previous defined n + 1), or check the configured `n` that way in `branch_checks`. |
| R6C-6 | m | `v8.py:630-633`, `:656-677` | A cell's own verdict "undefined ..." is taken on the PM's word for any cell: nothing checks that it was not run (a paired file or NAV dir exists), nor that the ruling it names applies to that cell (PM4-10 applies to a composition fit). A run cell recorded "undefined" disappears from the ladder, the parent chain and N. | Refuse an own-verdict undefined cell whose configured `paired` file exists. |
| R6C-7 | m | `atx-impl/src/strategy_ic_admission.cpp:654-657` | `composition_recorded_rule` skips a file without a string `provenance.rule`: deleting (or nulling) `provenance.rule` lets any row's block through, the R6B-C-5 scenario by another spelling. Reachable only by a hand-edited file (the lock pins the fitter's output). | Require a string `provenance.rule` whenever `theme_standardise` is present (the hand-written test files would need one). |

No I finding.

## Scope verdicts

### 1. Tie rule PM4-12

a. MATCH (tie step); the residual feeding it is computed by different algorithms by design.
- Block detection: C++ `strategy_ic_theme_resid.cpp:51-55` sorts `row = (own[offset+support[k]], k)` (z_t, support
  position) with `std::sort` (keys unique, so order deterministic) and runs while `row[end].first == row[b].first`.
  Python `composition_resid.py:275-280` `sorted(range(n), key=(float(z[k]), k))`, run while
  `z[order[end]] == z[order[b]]`. Same key, same exact `==` against the block's first element.
- Mean: C++ `:57-59` `sum = e[first]; sum += e[next] ...; sum / static_cast<f64>(end - b)`; Python `:282-285`
  `total = float(e[first]); total += float(e[k]) ...; total / float(end - b)`. Same start, same ascending name order,
  divisor = block size, both f64.
- Re-rank: C++ `cb::for_each_centered_rank` (`group_rerank.hpp:47-54`, `(b + e - 1) / (2(n-1)) - .5`); Python
  `centred_tied_ranks` (`:252-254`, `(less + less + equal - 1) / (2(n-1)) - .5`), numerators identical (less = b,
  equal = e - b).
- NaN: support excludes NaN on both sides (C++ `:73-74`, Python `:309`); z_t on the support is finite (ranks).
- Spanned test before the tie step on both sides (C++ `:93-94`, Python `:316-319`); first theme returns before it
  (C++ `:77-79`, Python `:312-314`).
- Divergence that remains (not a defect, pre-existing): Python's residual is `numpy.linalg.lstsq`, C++'s is MGS with
  re-orthogonalisation; the codes agree on the fixtures only because every residual order has a relative gap >= .0049.
  The "Python fitter path" never computes the blend (it writes `{rule, order}`); the Python side is the test reference.

b. MATCH. C++: with no equal z_t every run has length 1, the `if (end - b > 1U)` branch never runs and `s.dependent` is
not written; `row` is cleared and refilled exactly as before (`:95-97`), capacity reserved. Python: `np.array(e)` copy,
written only inside blocks of >= 2. Singletons are never divided (no x/1 path at all). Theme 0 untouched. Pinned by
`ThemeResid.NoTieCompositeIsTheRegisteredRuleBitForBit` (`:362`, `same_bits` against a pre-PM4-12 re-implementation on
the engine kernels) and the Python uint64-view test.

c. PARTLY. Hand check of `TiedCompositeStaysTiedAfterResidualisation` (6 names, W .5/.5):
- date 0: z0 = [-.1, -.3, .5, .3, -.5, .1], z1 = [-.2, -.2, -.2, .4, .4, -.2]; beta = -.12/.70 = -6/35, intercept 0;
  e = z1 + (6/35) z0 = [-.2171, -.2514, -.1143, .4514, .3143, -.1829]. No tie step: ranks [-.3, -.5, .1, .5, .3, -.1],
  out = [-1/5, -2/5, 3/10, 2/5, -1/10, 0] = TIE_SPREAD. Rule: block means -.1914 / .3829, re-ranked -.2 / .4 (= z1),
  out = [-3/20, -1/4, 3/20, 7/20, -1/20, -1/20] = TIE_EXPECTED.
- date 1: beta = -.48/.70 = -24/35; e = [-.4686, .0571, .2057, .1943, -.0571, .0686]; block means level0 -.2629,
  level2 .1257, level1 .1371 (order 0 < 2 < 1), re-ranked -.4/0/.4; out = [-1/4, -1/4, 7/20, -3/20, 1/20, 1/4] = pinned.
- Fails under: no block mean (out = spread, >= .05 away) YES; block mean computed but the raw residual ranked (same as
  no block mean) YES; "average the ranks, no re-rank" YES (date 1: -.4/.1/.3).
- Does not fail under: tolerance detection on z_t, and cannot: z_t lies on the lattice k/(2(n-1)) - .5, so any
  tolerance below 1/(2(n-1)) (~8e-5 at n = 6,000) gives the same blocks; the rule is tolerance-invariant on z_t. Also
  not caught: block sum instead of mean, and "average ranks then re-rank" (R6C-3).

### 2. Theme order PM4-11 / R6B-O-4: MATCH (with R6C-4)
- `registered_order` (`composition_resid.py:101-111`) refuses a list whose first ten are not `FROZEN_PREFIX` in order or
  that repeats a theme; later themes keep list (registration) order; `theme_order` restricts to weighted themes and
  refuses an outside theme. Source is the constant `PRIOR_THEMES` (fitter `:2416`), never a statistic.
- C++ copy `strategy_ic_theme_resid.hpp:23-26` = the ten + `filing_events` (11); Python `PRIOR_THEMES` = registry = the
  ten today. Not identical but a superset whose extra entry is unreachable: the fitter cannot weight `filing_events`
  until it is registered (prior metadata refuses an unregistered theme, `:539-541`). The pin
  (`test_composition_resid.py:216-224`) asserts `cpp[:len(PRIOR_THEMES)] == PRIOR_THEMES` and
  `cpp == FROZEN_PREFIX + ("filing_events",)`, so a different later theme fails loudly.
- Runner: `composition_residualise` (`strategy_ic_theme_resid.cpp:156-192`) refuses a weighted theme outside the list,
  then a non-permutation, then any order other than the list restricted to the weighted themes; runs in
  `composition_weights` (`strategy_ic_admission.cpp:715`) before any role payload. `BlockRefusalsPrecedeAnyPayloadOrOutput`
  (`strategy_ic_runner_test.cpp:3183`) checks plan-only and full runs: error, empty log, no output dir. Order recorded in
  recipe and both combined manifests only when the block is present (flag-absent bytes unchanged).

### 3. Pooled fit PM4-7: MATCH
- `POOLED_COMPOSITIONS` (`fit_composition_weights.py:246-248`) gains both ic-shrink ids; pooled runs go
  `fit` -> `pool_eras` -> the same `fit_prior` (`:2026-2029`), whose ic-shrink branch (`:2337-2341`) and
  `composition_resid.apply` (`:2416`) are the single-window code; aim records via `AIM_RULES` (`:239`, `:2002`).
- `POOLED_THEME_RESID` (`:251`, check `:1957-1960`) refuses any other `--theme-resid` id by name; unknown
  `--composition` refused by name (existing).
- One-era equality: `test_pooled_ic_shrink_fit_over_one_era_equals_the_single_window_fit` (both ids) and
  `test_pooled_theme_resid_over_one_era_equals_the_single_window_fit` (four parents); two-era ic-shrink against
  `composition_ic_shrink.ic_shrink` on the pooled admission. No two-era theme-resid test (the block is copied into each
  era file by `dict(document, ...)`, `:2421-2422`, by reading).
- aeda5bd7: `load_resid_parent` (`:436-454`) refuses `--theme-resid-parent` with `--era`; `research_cycle.py:1295` never
  passes it when `self.fit_pool` is set (every history read: `research_roles.py:198`, `:251`); the pooled fit still takes
  `--theme-resid theme-resid-v1`. An OD-3 read of a V8-F carrying R-11 is not blocked; the parent's weights file stays a
  pinned input that exists. Single-window cells are never `roles:` specs (checked base-lo1/lo3, r1, r2, r7, r8, r10).

### 4. Ladder criteria PM4-8 / PM4-9: MISMATCH (R6C-1, R6C-2; minors R6C-5, R6C-6)
- Directions: R-8 `lo <= v <= hi` with [.04, .06] (`v8.py:456`; E-43 closed [.8, 1.2] x 5%, every TRAIN year via
  `train_years()`); R-10 / R-11 `le` on tau per gross; R-12 `le` on tau; rule 5 `dsr > 0` strict (`:767`). Correct.
- Artifacts: R-8 reads the cell's own v8.summ row `year_table[].ann_vol` (no parent read); R-10/R-11/R-12 read cell and
  parent v8.summ rows; dSR from `v8.cells[].paired` with names checked (`paired_of`).
- Branch: config `defined_if` R-9a..c rejected [R-6] (E-38, E-37); R-10, R-11 accepted [R-6, R-1] (E-38, E-45); R-12
  accepted [R-6] (E-38); R-8 always (E-38 slot 48). `cell_states` (`:656-677`) waits while a named cell is open.
- Undefined cells: `_undefined_row` reads nothing, renders "undefined (ruling)", excluded from `inputs()` (`:1594`) and
  the default year table (`:325`); never a parent (`NEVER_PARENT`); a decided verdict on one refused (`branch_checks`).
  Report-only R-9: no rule, accept/reject refused.
- Parent = last accepted: `parent_checks` with undefined/report counted settled. Correct.
- Recorded verdict vs rule: compared (`verdict_rule_checks`), but an accepted cell with an n/a rule from a missing
  computed input is not refused (R6C-1). The registered statistic for R-10/R-11 is not the computed one (R6C-2).

### 5. R-11 parent check (R6B-O-5, PM5-1, PM5-2): MATCH
- Template path: `research_spec.py:191` derives `reference_resid_parent` = parent's `fit.output/composition_weights.json`
  whenever the template's own fit flags add `--theme-resid` as a string (r11.json does); `lock --write` pins it;
  `research_cycle.py:1295-1303` passes `--theme-resid-parent PATH --theme-resid-parent-sha256 PIN` and binds it.
- Paths that fit `--theme-resid` without the check: a `fit_pool` (history read, PM5-2); a template that lists
  `inputs.reference_resid_parent` under `unset` (`research_spec.py:193`; r11.json does not); a hand-written plain spec
  (PM5-2: no cell is fitted by a bare command); descendants (NAV-only child, add-alpha R-12) are re-fits of other inputs.
  No other path found; add-alpha drops every `reference_` input (`research_add_alpha.py:160`) and never builds steps on
  its `verify=False` cycles.
- Strictness: `parent_check` (`composition_resid.py:171-205`) removes only the block, `provenance.resid`,
  `provenance.script_sha256`, and `provenance.admission_sha256` after proving the two admission tables equal apart from
  `inputs.script_sha256`; key presence strict. The weights provenance and admission carry no output path, timestamp or
  computed/reused count (`fit_composition_weights.py:2232-2264`, `:2362-2395`), so a genuine re-fit on the same commit
  passes; module SHAs strict per PM5-1. Not too loose: every weight-affecting change shows in the compared output.

### 6. R6B-C-5 and R6B-C-4: MATCH (minor R6C-7)
- C-5: `composition_recorded_rule` (`strategy_ic_admission.cpp:654-675`) checks both directions for all three rows,
  admits the R-1 identity device (rerank false, recorded ew-theme-v1) only; runs before `composition_residualise` and any
  payload. Test `:3546` covers 7 admitted, 11 refused, plan-only and full run. Existing hand-written test files carry no
  `provenance.rule`, so they are unaffected.
- C-4: `argparse_values` / `validate_e27b` (`scripts/research_cycle.py:373-417`) read the pair, `=`, every prefix
  (counted even when ambiguous), repeats, stop at `--`; v8 = `verdict: true` or `v8` in any spelling of `--protocol`;
  non-two-token spellings refused in v8 (and `--protocol` everywhere). Called from `validate_spec`, reached by
  plan/run/status (`load_spec`, `make_cycle`), template lock (`load_spec`), roles plan/run/lock (`derived_spec` ->
  `validate_spec`), add-alpha (`research_add_alpha.py:352`). No spelling found that passes.

### 7. Compile risk and test premises
- Likely compile failures: 0. Checked: new signatures (`std::span<const std::string>` default `{}`, both call sites),
  `theme_order_json` in `ic_detail` (not the unnamed namespace), `StandardiseRule` (5 members, only one initialiser
  list), string/string_view comparisons, lambda captures, `D`/`N` are `usize`, structured bindings used only in gtest
  macros, local aggregates (`Decided`, `Book`) and the 10-initialiser `BookDecision` (same form as existing lines 293,
  365, 549), helpers moved into a named-namespace header with `inline` (no clash with any `atx` name). clang-cl `/W4`
  carries no `-Wshadow` / `-Wconversion`.
- Premises:
  - O-7 small kernel case: hand-checked z0 = [-.25, 0, .5, .25, -.5]; theme 1 beta -16/15, residual
    [.1, -.3, -.1, .3]; theme 2 betas 130/261 and -46/87; out = [-1/24, -7/20, 3/40, 17/40, -13/120] as pinned; (c)
    moves names 0 and 1 by exactly 1/12. Would fail under (a)-(c).
  - O-7 runner 3-cycle: hand-checked all eight values (-3/35, 1/20, -2/5, -13/140, 3/140, 19/140, 1/4, 17/140). The
    3-cycle position map makes the inverse map another rule; would fail under (d). Unverified premise: the VM gives a
    non-finite value for `1 / 0` on name 0 and the composition drops it.
  - S-1 (a) `SpoV3.TieredRunVoidsOnItsSwapFinancedPrimaryBookOnly` (`strategy_spo_v3_test.cpp:588`): removing
    `set_primary_book` leaves the engine primary at tiered[3] (S2 x flat-300): `run.primary` mismatches and index 3
    voids instead of 1. Fails as claimed; the ADV-0 unmet premise is asserted, so it cannot pass vacuously.
  - S-1 (b) CLI void test (`:690`): would fail on a broken void path (exact file list, exit 3, keys). Unverified: that
    ADV 0 from decision 93 produces an unmet primary row and exit 3 rather than an error.
  - S-2 `TradedAfterIsBackFilledPerBookAcrossCadences` (`:1039`): an engine-wide fill writes A's d=2 row from B's flat
    book (all zero, Pearson NaN) where the per-book value is finite; fails as claimed. Unverified at run time.
