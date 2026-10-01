# Lane FIX-3 brief: residuals of the scoped re-review (review-w1-fixes.md)

Read first: `lane-rules.md` (binding). Worktree `C:/atx-wt/pool-10`; first command
`git checkout -b feat/platform-v8-fix3-20260930 fd2ff7a8`. Report `task-FIX-3-report.md` (commit `git add -f`).
Findings: `review-w1-fixes.md` (F-1..F-14). Rulings: E-27, E-27a, E-37, prereg items 2, 5, 7 in `v8-prereg.md`.
One commit per finding, `fix(<area>): <what> (review F-n)`. Order: F-8, F-1, F-9, F-10, F-14, then the minors.

- F-8: a child derived by add-alpha from a labelled parent (spec `inputs.label_role`) inherits the label role in
  every phase that marks the book (`ref`, NAV, capacity curve, diagnostics); the `ref` comparison against the
  parent's labelled S2 daily CSV is made on equal footing. Test: derive from a labelled fixture spec; the ref argv
  carries `--label-role` and `--label-role-sha256`.
- F-1: a `rerun_of` must name a cell of the same kind whose own line carries a `defect:` line with a ruling id;
  one re-run per defect line (a second re-run of the same target is refused); the re-run line pins the target's
  cell id and trial id; a legacy v7 trial id as target is refused unless its line exists in the ledger. Tests for
  each refusal.
- F-9: a template cell's `spec_sha256` and the C-13 binding cover the resolved spec (template plus every parent in
  the chain, by content digest), and a matching spec digest never skips the argv check. Test: editing a parent
  changes the child's digest; a stale NAV is refused.
- F-10 (Ruling E-27a): `ew-theme-aim-v1` renormalises gains inside each theme (share 1 / T) and applies the member
  cap 1 / (2T) after, as `ew-theme-std-aim-v1` does; share the implementation, no copy. Test on a two-theme fixture.
- F-14: the `r6` template keeps `--capacity-curve` (E-37).
- Minors F-2..F-7, F-11..F-13: fix each whose fix is local to a file you already edit; list the rest untouched.

Files you own: `scripts/research_cycle.py`, `research_add_alpha.py`, `cycle_resume.py`, `research_spec.py`,
`backtest_integrity.py` (wherever it lives; `git ls-files | grep backtest_integrity`), `fit_composition_weights.py`
aim-variant path only, `scripts/specs/v8/*.json`, their tests. Lane ERA (pool 3) edits the era pooling modules and
the pooled path of `fit_composition_weights.py`; lane FIX-2 (pool 7) edits report tooling, `alpha_report_card.py`,
`research_fields_holdings.py`, spo-v3 capacity-curve C++. Do not touch those. Run pytest for every test you touch.
