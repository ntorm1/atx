# Expansion X lanes (owner goal of 2026-10-02; Rulings PM7-1..PM7-5 in `progress.md`, "PM session 7")

Owner goal: finish v8 and make real progress on alpha generation: new orthogonal alpha DSL signals, better
existing ones, better combinations, new datasets; Sharpe, deflated Sharpe, capacity and returns all up.

## Rules for every X lane

1. Read `lane-rules.md` first (binding: never build C++, never run real data, never dispatch subagents, files with
   the Write / Edit tools, commit per item with the Opus trailer, report committed with `git add -f`). Then
   `docs/plans/2026-10-02-platform-v8-status-6.md` sections 1 and 2, `v8-prereg.md`, and "PM session 7" in
   `progress.md`. For C++: `.agents/cpp/agent.md`.
2. Blind work only. Do not open any return, IC, Sharpe, turnover or NAV output of the 2020-2023 window
   (`build-equity/` NAV, cards, marginal, admission and diagnostics files are closed to you; manifests, receipts
   and field lists are open). The numbers already in status 6 and the ledger are public. Nothing dated 2024-01-01
   or later is opened. A choice made after looking at a 2020-2023 result is a trial; you have no trials.
3. One variant per hypothesis. Every constant (window, lag, half-life, cap, shrinkage) is fixed by you, blind,
   from the literature or from a stated mechanical argument, and written in the report as the registration.
   No grids, no "to be tuned".
4. Never touch `atx-db/` in any worktree. Pool 10 belongs to another session: do not enter it. Other branches are
   read with `git show <sha>:<path>` / `git log <sha>`: LIB3 / FIELDS-V9 `834d5a05`, LIB2 `5d64644e`, COMB2
   `1e8af5b8`, ORTH `c1cc57ce`, MINE-RUN `6ea76460`, mining `1bd448cd`.
5. Code is flag-gated; flag absent is byte-identical; say how root verifies it. Python tests on synthetic data
   are run by you; C++ is written to compile first time and is not built.
6. Final reply to the PM: at most 15 lines (status, commit SHAs, one test line, the count of candidates, concerns).

## Lane XSIG: new signals, orthogonal by construction

Read `library-v8-draft.md` (sections 0, 4, 5, 8, 10), `task-LIB2-report.md`, `task-F-3-report.md`, the v8.0 roster
and its 10 themes (`scripts/specs/v8/lib-v80.json` and the library files it names), the fields specs v10 / v11 /
v12, the DSL op catalog (`atx-engine/src/factory/op_catalog.cpp`), and the v9 draft on `834d5a05`.
Deliver the X screen set: at most 12 new candidates, ranked by your prior of marginal contribution. For each:
canonical definition, literature sign and citation, ONE frozen DSL string, theme (existing or new), tier, fields
needed, and the construction argument for orthogonality (a data source or a horizon that no roster theme uses;
name the nearest roster member and why the candidate is not a re-statement of it). Prefer, in order: (1) fields
already in the store that no roster member reads; (2) v9 draft items already FIELD-BUILT (`nt_late`,
`earn_season`); (3) conditioning or interaction forms with a published basis. Candidates already allocated to
R-7 (v8.1) or R-12 (v8.2) are excluded. A candidate that needs a new field gets its builder in
`atx-engine/tools/research_fields_*.py` (point-in-time, reader-side seal from `research_window`, reuse inputs
complete per findings B-1 and N-1), a fields spec entry, and synthetic tests (oracle computed in the test, a
look-ahead probe that fails). Write the frozen `add-alpha` command lines and registry rows.
Report: `task-XSIG-report.md`.

## Lane XIMP: repairs and refinements of existing signals

Read the same library material, `code-review-v8-signal.md`, `review-w1-*.md` findings on signals, and every DSL
string of the v8.0 roster (52) against its canonical literature definition.
Deliver two lists. (A) Defect repairs: a roster string that does not compute its stated canonical definition
(wrong lag, wrong denominator, missing skip period, wrong sign convention, look-ahead margin larger than the
filing lag needs). For each: the defect line, the corrected frozen string, why it is a defect and not a taste.
(B) Refinements, at most 8: one frozen replacement string per member, each with a published basis (examples of
the class, not a list to copy: industry-relative form, volatility scaling, residual form, announcement-window
exclusion, intermediate-horizon form). For each: the member replaced, the string, the citation, the prior on
sign and on which cost it trades against (turnover, breadth). Also (C), at most 2 library-wide processing
variants with a published basis (for example a per-theme decay half-life by signal speed), each one frozen
rule, with the code path that would carry it (flag-gated) named but written only if it is Python.
Report: `task-XIMP-report.md`.

## Lane XCOMB: combination and capacity rules

Read `task-R-1-report.md`, `task-R-10-report.md` (`ic-shrink-v1`), `task-R-11-report.md` (`theme-resid-v1`),
`task-R-6-report.md` (spo-v3), `task-R-8-report.md` (risk target), `task-R-4-report.md`, `task-R-5-report.md`,
Rulings E-26, E-27, E-38, and `review-w1-T.md` T-1. R-3 (aim rule) and R-4 (trade band) were not accepted; do not
re-propose them in another form.
Deliver at most 3 new rules, each one hypothesis, constants fixed blind: (1) one combination rule that R-10 and
R-11 do not cover (for example theme shares from the risk store's covariance of the theme composites, equal risk
contribution); (2) one capacity rule (for example a liquidity-scaled position cap, or a signal half-life by
theme speed applied before the aim) with the mechanical criterion that would judge it (net Sharpe at 4x NAV,
cost per traded dollar); (3) free, your best prior. For each: C++ rule in its own file beside the existing rules
and registered in the rule table, the Python fitter path if the rule has one, a cell template
`scripts/specs/v8/x-<rule>.json` following the A2 templates (parent = last accepted, PM6-6 gross matching
applies), tests (closed-form fixture, Python equals C++, flag absent identity).
Report: `task-XCOMB-report.md`.

## Lane XDATA: datasets

Read `task-F-0-report.md` to `task-F-3-report.md`, the fields specs, the four data asks to atx-db (OD-6; status 5
section on data asks), and the warehouse work now on `origin/main` (`git log origin/main -- atx-db | head`,
`git show origin/main:atx-db/docs/<file>` for the documents; documents, schemas and source only, never a data
file, never the `atx-db/` working tree).
Deliver: (1) a dataset map: source, what it holds, point-in-time lag, coverage window, which v8 fields read it,
what is unused; (2) the status of the four data asks against the new warehouse (gold alpha panels, 13F archive,
FINRA short volume, shares outstanding, fundamental statements, XBRL catalog); (3) the 8 field families with the
best prior of alpha orthogonal to the 10 themes, ranked, each with the builder design (stamping rule, seal,
reuse fingerprint) and the candidate signal it serves; (4) builders and synthetic tests for the top 3 that need
no atx-db change (`atx-engine/tools/research_fields_*.py`, a fields spec entry each); (5) at most 5 external
datasets not in house (source, cost, history depth, point-in-time quality, the signal class they open) as owner
decisions. Coordinate nothing with XSIG: XSIG owns signals on existing fields, you own new fields.
Report: `task-XDATA-report.md`.

## Lane XPRE: pre-registration of X and of the mined campaign

Read `v8-prereg.md` in full, Rulings E-34, E-38, E-45, PM5-23, PM6-6, PM7-1..PM7-5, `task-V8-F-brief.md`, the
MINE-RUN report and registration on `6ea76460`, `review-mine.md`, and the OD-7 lines of `progress.md`.
Deliver `v8x-prereg.md` (sprint directory), complete and ready for the PM to rule on: baseline (the book at
V8-F); windows and the seal (unchanged); trial counting (continues from N at V8-F; what counts as a trial, an
admission trial, a calibration run, a campaign evaluation); the X budget, proposed by you with the argument
(construction cells, admission trials for XSIG / XIMP / XDATA candidates, the campaign's `--budget` and memory
cap); the order of X cells; the acceptance rule per cell and the cumulative test at the end; the deflated Sharpe
(formula, the count it uses, the variance of trial Sharpes and where it is read from; print the value for the
hand-written set alone and for the total, PM7-2); the leverage cell of PM7-3 with its mechanics limit; the
hidden-block gate (`holdout_gate.py`, OD-3) and when it may be used; what voids a cell. Also the runbook for the
campaign: exact argv, what is checked before any return is read. State every open choice as a recommendation
with its cost if wrong; the PM rules.
Report: `task-XPRE-report.md` (short; the deliverable is `v8x-prereg.md`).
