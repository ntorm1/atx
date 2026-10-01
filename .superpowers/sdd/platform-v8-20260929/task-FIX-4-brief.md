# Fix round FIX-4 (after the integration 6 part B re-review; two lanes, disjoint files)

Binding first read: `C:/atx-wt/pool-2/.superpowers/sdd/platform-v8-20260929/lane-rules.md`. Both lanes branch from
the integration 6 part B head of root (given in the dispatch). No statistic of 2020-2023 has been read; every
ruling below was declared before any read. The findings are in `review-6b-spo.md`, `review-6b-comb.md`,
`review-6b-orth.md` (paths in the dispatch); read the rows named for your lane in full, they carry file:line,
the failing scenario and the smallest fix. One commit per finding. C++ is never built by a lane.

## Rulings that bind this round (also in progress.md once folded in)

- PM4-7: the pooled (era) fit implements every composition a V8-F can carry: `ic-shrink-v1`, `ic-shrink-aim-v1`
  and `theme-resid-v1` (on each of its parents) join it, through the same shared functions as the single-window
  fit; the one-era equality test covers them.
- PM4-8: the v8 ladder (pitch config, scorecard template, `mega_report/v8.py` ladder checks) gains R-8 (rule 5
  plus realised volatility inside [.8, 1.2] x 5% in each TRAIN year, E-43), R-9 (three report-only frontier
  cells, no acceptance), R-10 and R-11 (rule 5 plus planned turnover per unit gross not higher than the parent,
  E-44), R-12 (PM4-9).
- PM4-9: R-12 is accepted whole on rule 5 plus "book turnover not higher than the parent" (as R-2 and R-7 under
  E-36).
- PM4-10: a fit refused because the 1/(2T) cap is infeasible is a cell that cannot be formed (ledgered as
  undefined, adds 0), not a rejected trial. No code change needed beyond a clear refusal message.
- PM4-11 (R6B-O-2, theme order): the registered order of `theme-resid-v1` is the registry order of
  `PRIOR_THEMES` with any theme registered later appended in the order it was registered; `filing_events` is
  therefore last, after `ownership_flow`. The order is derived from `PRIOR_THEMES` with a frozen-prefix check
  (the ten original themes in their registered order, then later themes), never from a data statistic.
- PM4-12 (R6B-O-3, ties; the registered text is silent on ties; clarified before any read, id unchanged because
  nothing was ever run or written under it): names that are tied in theme t's own standardised composite z_t
  stay tied after residualisation. The residual is replaced by its mean inside each exact tie block of z_t, and
  that block-mean residual is re-ranked (centred tied rank). A composite without ties gives the registered
  result bit for bit. The first theme is still unchanged bit for bit. Python fitter and C++ kernel implement
  the same expression in the same order; a fixture with a tied composite (a sparse flag theme) pins it.

## Lane FIX-4a (pool 4): the theme-resid rule and the pooled fit

Owns: `atx-impl/tools/composition_resid.py`, `atx-impl/src/strategy_ic_theme_resid.{hpp,cpp}`, the theme-resid
tests (Python and C++), `scripts/specs/v8/r11.json`, the pooled-fit part of
`atx-impl/tools/fit_composition_weights.py` (`POOLED_COMPOSITIONS` and its dispatch), the rule table in
`atx-impl/src/strategy_ic_admission.cpp`.

1. R6B-O-1: `resid_block` accepts any rerank-true block whose rule is in the runner's table (`ew-theme-std-v1`,
   `ic-shrink-v1`, `ic-shrink-aim-v1`); `composition_resid.apply` runs after both attach calls. The integrator
   already changed this at `393910ed`: read that commit first, keep what is right, finish what is missing, and
   add fitter tests on an ic-shrink and an ic-shrink-aim parent asserting `parent_composition`.
2. R6B-O-2 (PM4-11): theme order derived as ruled; pin test extended; `filing_events` last.
3. R6B-O-3 (PM4-12): tie-preserving residual in the Python fitter path and the C++ kernel; fixtures with a tied
   composite in both; state in the report how the tie block is detected (exact equality of z_t) and show the
   no-tie bit identity.
4. R6B-O-4: the runner checks the weights file's `order` against a C++ copy of the registered list restricted to
   the weighted themes, and records `order` in the recipe and the combined manifest.
5. R6B-O-5: under `--theme-resid` the fitter takes the parent's weights path and SHA, requires the re-fit to
   equal the parent file apart from the resid block, `provenance.resid` and `script_sha256`, and records the
   parent SHA. `r11.json` passes them from the parent cell.
6. R6B-O-6, R6B-O-7: the aim-parent end-to-end test; a small-n kernel case and a three-theme runner case that
   fail under (a) regressing on residuals, (b) on re-ranked residuals, (c) no intercept, (d) an inverted
   position map.
7. R6B-C-1 (PM4-7): pooled fit for `ic-shrink-v1`, `ic-shrink-aim-v1` and `theme-resid-v1`; one-era equality
   test for each. Any id the pooled path does not implement is refused by name (existing behaviour).
8. R6B-C-5: the runner refuses a weights file whose recorded rule differs from its block's rule (every row of
   the rule table), before any payload or output.

## Lane FIX-4b (pool 10): ladder criteria, the E-27b guard, spo tests

Owns: `atx-impl/tools/mega_report/v8.py` and its tests, `docs/plans/mega-alpha-v8-pitch.config.json`,
`docs/plans/mega-alpha-scorecard-v8.template.md`, `scripts/research_cycle.py` (`validate_v8_keys` only),
`scripts/tests/test_research_spec.py` (your tests only), `atx-impl/tests/strategy_spo_v3_test.cpp`.

1. R6B-C-2 (PM4-8, PM4-9): ladder entries and computed checks for R-8, R-9, R-10, R-11, R-12, in the style
   FIX-2 used for P-2 / P-3 (criterion computed from the cell's own artifacts; recorded verdict compared with
   the computed rule; parent = last accepted). The E-38 / E-45 branch is part of the ladder: R-9 present only
   when R-6 was rejected; R-10 / R-11 present only when R-6 and R-1 were accepted; a skipped cell renders as
   "undefined (ruling id)", never as unavailable.
2. R6B-C-4: the E-27b refusal reads the parsed argv (`--composition ew-theme-aim-v1`, `--composition=...`, any
   accepted abbreviation, `--protocol v8` and `--protocol=v8`), at plan, run, lock and add-alpha. Tests for each
   spelling.
3. R6B-S-1: a test that fails if the tiered primary-book label line (`strategy_nav_v7.cpp:510`, `run_scenarios`
   setting the primary from the run's own matrix) is removed: a tiered run whose S2 x `swap-fin-v1` book has an
   unmet row voids. And a test of the CLI void path: exit 3, `v7_extras.json` with `status: void`,
   `voided: limits_unmet` and the `limits_unmet` block, no NAV or returns file. Tests only; if the seam cannot
   be reached without a source change, make the smallest testable seam and list it under cross-lane edits.
4. R6B-S-2: the E-14a back-fill tested on two books with different cadence, so an engine-wide (not per-book)
   back-fill fails.

## Report

`task-FIX-4a-report.md` / `task-FIX-4b-report.md` in your worktree's sprint directory, `git add -f`: per finding
the commit, files, what root builds and which gtest filter and pytest files verify it, deviations, cross-lane
edits, open risks. Final reply short: status, commit per finding, head SHA, one line of pytest results, concerns.
